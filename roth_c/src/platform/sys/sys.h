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

#include <stddef.h>   /* size_t */

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

/* ---- the game thread ---------------------------------------------------------
 * The game runs on its own thread while the main thread owns the window/present
 * loop. The tick preempts the game thread cooperatively, exactly as the original
 * timer interrupt preempted the single-threaded game, so the game thread is the
 * only one the tick ever touches — no new races on the tick-shared globals.
 *
 * sys_spawn_game_thread starts `fn(arg)` on a fresh thread with a stack of at
 * least `stack_bytes`, and arranges that the tick machinery can name and preempt
 * exactly this thread (and no other). It returns 0 on success, non-zero on
 * failure (in which case no thread was created and the caller runs `fn` itself).
 * sys_join_game_thread blocks until that thread returns. */
int  sys_spawn_game_thread(void *(*fn)(void *), void *arg, size_t stack_bytes);
void sys_join_game_thread(void);

/* Called once at the very top of the game thread, before it enters the game.
 * Where the tick is delivered as an asynchronous per-thread event this arms that
 * delivery for this thread and captures the thread's context the tick needs;
 * where the tick is produced by other means it does nothing. */
void sys_game_thread_enter(void);

/* ---- dynamic shared-object loading -------------------------------------------
 * The plugin loader opens one shared library per mod, resolves its single
 * versioned query export, and keeps the handle for the life of the process. The
 * four calls below wrap the OS loader so the loader code stays OS-independent.
 * sys_dlopen binds every symbol immediately and keeps the library's symbols
 * private to this process image (no global-namespace leakage). sys_dlerror
 * returns a human-readable description of the most recent loader failure, or NULL
 * if there was none; it is cleared by reading it, so the loader clears it before
 * a lookup and re-reads it after to tell "resolved to NULL" from "not found". */
void       *sys_dlopen(const char *path);
void       *sys_dlsym(void *handle, const char *symbol);
void        sys_dlclose(void *handle);
const char *sys_dlerror(void);

/* The base filename of a plugin's shared object inside a mod folder ("plugin.so"
 * or the platform's equivalent), so the discovery loop carries no per-OS name. */
const char *sys_plugin_soname(void);

/* Allocate `len` bytes of read/write, page-granular, anonymous memory whose
 * address fits in 32 bits (some far-pointer consumers keep only the low 32 bits
 * of an address). Returns NULL on failure. */
void *sys_lowmem_alloc(size_t len);

/* Change the protection of an executable code range to open a brief writable
 * window around a self-modifying-code patch and then restore it. SYS_PROT_RWX
 * makes the range writable-and-executable for the write; SYS_PROT_RX restores
 * read-and-execute afterward (flushing the instruction cache where the platform
 * requires it). Returns 0 on success, non-zero on failure. */
enum sys_exec_prot { SYS_PROT_RWX, SYS_PROT_RX };
int sys_protect_exec(void *addr, size_t len, enum sys_exec_prot prot);

/* The directory containing the running executable (no trailing slash). Falls back
 * to the directory of `argv0`, then to "." when neither is available. The returned
 * string is owned by the seam and stays valid for the life of the process. */
const char *sys_exe_dir(const char *argv0);

/* Return non-zero if `dir` contains an entry whose name matches `name`
 * case-insensitively; 0 if `dir` cannot be opened. */
int sys_dir_has(const char *dir, const char *name);

/* Enumerate the entries of `dir`, invoking cb(name, ud) with each entry's plain
 * name (no path). Each caller applies its own filtering (for example skipping the
 * "." and ".." entries). If cb returns 0 the walk stops early; a non-zero return
 * continues it. Returns 0 on success (including an early stop), -1 if `dir` cannot
 * be opened. */
int sys_enum_dir(const char *dir, int (*cb)(const char *name, void *ud), void *ud);

#endif /* ROTH_SYS_H */
