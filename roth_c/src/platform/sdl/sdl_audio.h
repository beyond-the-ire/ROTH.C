/* sdl_audio.h — the in-process SDL3 audio consumer API for the moddable engine binary (roth) (task #102 / M2;
 * docs/SDL3_FOLD_DESIGN.md §2.5, §4).
 *
 * SDL-free header (declares only entry points), so the SDL present TU (sdl_host.c) can drive the
 * audio consumer without either file pulling in the other's implementation headers. The
 * implementation (sdl_audio.c + the isolated sdl_tsf.c) links ONLY into the moddable engine binary (roth) — the
 * trap host / oracle never compiles or links it (design §2.6, §7). ROTH_STANDALONE-only by linkage.
 *
 * M2 is a CONSUMER port: it drains the SAME producer mailboxes the external SDL2 viewer consumed
 * (the /roth_audio PCM ring + /roth_midi event ring that audio.c publishes), now in-process, via an
 * SDL3 SDL_AudioStream get-callback. The audio *producers* in audio.c are untouched.
 */
#ifndef ROTH_SDL_AUDIO_H
#define ROTH_SDL_AUDIO_H

/* Try to open the in-process audio output. Idempotent + retriable: audio.c's audio_init() runs on
 * the game thread and may not have created /roth_audio yet when the SDL present loop first spins, so
 * this is called once per present frame until it succeeds (then it is a no-op). Must run on the main
 * thread (the thread that owns SDL), so the SDL audio thread it spawns inherits main's blocked signal
 * mask (SIGALRM/SIGTERM/SIGUSR1) — the game-thread tick never lands on the audio thread. */
void sdl_audio_poll_open(void);

/* Env-gated (ROTH_SDL_AUDIO_LOG) consumer-clock proof: periodically logs the PCM ring read cursor
 * (g_au->r) advancing + the callback invocation count. Zero cost when the env var is unset. Call
 * once per present frame from the main thread. */
void sdl_audio_log_tick(void);

/* Clean teardown on quit: stop + join the SDL audio callback (SDL_DestroyAudioStream), close the
 * SoundFont synth, quit the audio subsystem. No underrun spam, no hang. Call before SDL_Quit(). */
void sdl_audio_shutdown(void);

#endif /* ROTH_SDL_AUDIO_H */
