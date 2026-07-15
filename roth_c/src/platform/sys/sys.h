/* Per-OS system seam.
 *
 * A small set of primitives that the otherwise portable host layer needs but
 * that only the operating system can provide. Each supported OS supplies its own
 * implementation of the functions declared here: the POSIX bodies live in
 * sys_posix.c, the Windows bodies in sys_win32.c. Code above the seam calls
 * these names and stays OS-independent.
 *
 * Today the seam carries the periodic game tick. The game is driven by a ~70 Hz
 * timer interrupt (the original programmed the PIT to that rate); on each fire
 * the portable tick body roth_tick_isr() runs. The way a fire is produced differs
 * per OS — a POSIX interval timer delivering a signal, or a Windows timer thread —
 * but the body it drives is shared.
 */
#ifndef ROTH_SYS_H
#define ROTH_SYS_H

/* The portable per-tick body: runs once per timer fire, on the thread the tick
 * interrupts. It advances the frame clock, polls audio, drives the cursor, pumps
 * the cutscene decoder and drains the keyboard ring. It does no CPU
 * segment-register work of its own — where a platform needs the host TLS
 * selectors restored across the tick, that is handled by the OS-specific timer
 * code that calls this body. Defined in the host core (traps.c). */
void roth_tick_isr(void);

/* Arm the periodic tick at the given period (microseconds) and route each fire
 * to roth_tick_isr(). period_us = 14222 gives the game's ~70 Hz cadence. */
void sys_tick_start(unsigned period_us);

/* Retune the running tick to a new period (microseconds). Used when the game
 * reprograms the timer rate at runtime. */
void sys_tick_set_period(unsigned period_us);

/* Stop the tick (clean teardown). */
void sys_tick_stop(void);

#endif /* ROTH_SYS_H */
