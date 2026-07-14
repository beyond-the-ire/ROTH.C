/* plugin_loader.h — the image-free plugin loader / lifecycle (task #103; MODS_PLATFORM.md §10.4).
 *
 * Host-facing API ONLY (plain C types; the SDK types live entirely inside plugin_loader.c so the host
 * TUs that call these — boot.c / traps.c / main.c — need no SDK headers). Linked ONLY into
 * the moddable engine binary (roth); the trap host never compiles or links the loader.
 *
 * Lifecycle & boot order (design §10.3 / review finding 3 — ALL of this completes on the boot/game
 * thread BEFORE the SIGALRM timer tick and the audio thread start, so no plugin/mod callback ever
 * races a live tick):
 *     plugins_configure()             (main.c argv parse)
 *     [--list-mods] plugins_discover_report() -> exit
 *     plugins_load()                  discover+dlopen+validate+build api+report+on_load(MAIN)+seam
 *     plugins_dispatch_game_ram_ready()  on_game_ram_ready(MAIN) — the mods_apply moment
 *     ... engine runs ...             on_frame_game(GAME, via the int33 seam) / on_compose_tick(TICK_ISR)
 *     plugins_dispatch_unload()       on_unload(MAIN), reverse load order, after the game stops
 */
#ifndef PLUGIN_LOADER_H
#define PLUGIN_LOADER_H

#include <stdint.h>

/* main.c argv parse -> loader options (call once, before boot). dump = --dump-mod-chains. */
void plugins_configure(int no_mods, int strict, int list, int dump);
int  plugins_list_mode(void);     /* --list-mods requested: discover+validate+print, then exit */
int  plugins_nomods_flag(void);   /* --no-mods only: boot uses it to ALSO skip the legacy mods layer */

/* --list-mods path: discover + validate + print the resolved report; the caller then exits. */
void plugins_discover_report(const char *game_dir);

/* boot (game thread, PRE timer/audio/engine): discover + dlopen + validate + build the api table +
 * emit the resolved report + dispatch on_load(MAIN) + install the on_frame_game seam. Inert (no
 * discovery, no dispatch) when ROTH_MODS=0 or --no-mods. */
void plugins_load(const char *game_dir);

void plugins_dispatch_game_ram_ready(void);   /* on_game_ram_ready(MAIN) — game_ram staged pristine */

/* on_register_overrides(MAIN) for every plugin, then build the immutable chains + patch the pad
 * entries (task #103, override registry). Boot calls this in step 6b AFTER game_ram_ready and BEFORE
 * audio_init()/irq_timer_start() — single-threaded, pre-thread. Inert with no plugins (0 patches =
 * pristine call targets). Honors --strict-mods / --dump-mod-chains. */
void plugins_apply_overrides(void);

void plugins_dispatch_unload(void);           /* on_unload(MAIN), reverse load order, after game stop */

/* on_compose_tick(TICK_ISR): from shm_tick right after mods_compose_frame (traps.c, imgfree only).
 * Same 8-bit indexed pixel buffer + dimensions. No engine calls legal here (contract, not enforced). */
void plugins_dispatch_compose_tick(uint8_t *pixels, uint32_t width, uint32_t height);

/* on_scancode(TICK_ISR): the input seam, chained from iso_apply_scancode (traps.c) right AFTER the
 * legacy mods_filter_scancode. Runs each plugin's filter in load order; a 0 return SWALLOWS the key
 * (stops the chain). Returns the possibly-rewritten scancode (== sc when no plugin wants it). */
uint8_t plugins_dispatch_scancode(uint8_t sc);

#endif /* PLUGIN_LOADER_H */
