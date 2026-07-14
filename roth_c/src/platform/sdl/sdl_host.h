/* sdl_host.h — the in-process SDL3 window/input API for the moddable engine binary (roth) (task #102, M1).
 *
 * SDL-free header: it declares only the entry point main.c calls, so main.c pulls in NO SDL
 * headers (only sdl_host.c does). ROTH_STANDALONE-only by linkage — the SDL TU links into
 * the moddable engine binary (roth), never the trap host / oracle (docs/SDL3_FOLD_DESIGN.md §2.6, §7).
 */
#ifndef ROTH_SDL_HOST_H
#define ROTH_SDL_HOST_H

/* Run the SDL3 present + input loop on the CURRENT (main) thread. Reads the published
 * g_shm frame (palette->RGBA, correct aspect for every mode incl. 320x400 double-scan) and
 * writes the existing input rings (set-1 scancodes, relative mouse deltas + buttons), exactly
 * as the external viewer does — now in-process. `scale` = initial window scale (0 = auto-fit).
 * Returns 0 on clean shutdown (window closed / game exited), <0 if SDL failed to init. */
int sdl_present_run(int scale);

/* M3 auto-sanity (docs/SDL3_FOLD_DESIGN.md §6): probe whether SDL video can initialise (a display +
 * driver exist) WITHOUT committing the game thread. Returns 1 if a window is possible, 0 otherwise.
 * main() calls it before spawning the game thread in windowed mode so a display-less box fails with a
 * clear "use --headless" message rather than silently running windowless. */
int sdl_video_preflight(void);

#endif /* ROTH_SDL_HOST_H */
