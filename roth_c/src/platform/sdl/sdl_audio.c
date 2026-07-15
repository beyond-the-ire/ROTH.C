/* sdl_audio.c — the in-process SDL3 audio output for the moddable engine binary (roth) (task #102 / M2;
 * docs/SDL3_FOLD_DESIGN.md §2.5, §4).
 *
 * Absorbs the LOGIC of the external SDL2 viewer's audio path (viewer.c:94-188) as NATIVE SDL3: an
 * SDL_AudioStream opened with a get-callback that drains the host's PCM ring (/roth_audio) into the
 * device and mixes the MIDI event ring (/roth_midi) through a TinySoundFont SoundFont synth. This
 * is a CONSUMER port only — the audio PRODUCERS in audio.c (the digital mixer that fills the ring +
 * the MIDI capture that fills the event ring) are untouched.
 *
 * How the mailboxes are reached depends on how the producer (audio.c) backed them, a choice it makes
 * at boot to match the framebuffer backing:
 *   - in-process backing (the windowed default): the rings live in this process's own memory. We take
 *     the producer's own ring pointers (audio_pcm_ring/audio_midi_ring) and read them directly — one
 *     buffer, no second view. The producer writes ->w, our callback advances ->r, both on the same
 *     memory.
 *   - shared backing (the external-viewer publish path): audio.c shm_open()s /roth_audio + /roth_midi.
 *     We map those SAME objects a SECOND time in our own address space (the bytes an external viewer
 *     would map cross-process). Two MAP_SHARED views of one object are coherent, so the producer's ->w
 *     writes are seen by our callback's reads and our ->r writes are seen by audio.c's movie pacer.
 * Either way the producer is unchanged and we never touch audio.c's own g_au/g_midi globals; the
 * consumer-clock semantics (->r advance drives the GDV movie pacer) are identical.
 *
 * "BOTH"-mode rule (design §6, task brief): the in-process audio CLAIMS the ring — it is the sole
 * consumer that advances ->r. If an external roth-view is ALSO attached under --sdl, both would
 * drain ->r and split the sample stream (garbled audio + wrong pacing). Therefore, by rule, an
 * external viewer attached under --sdl is DISPLAY-ONLY: do not run its audio. (Nothing enforces this
 * in code — it's a documented operating rule, per "keep it simple, document it".)
 *
 * Threading (design §3): the get-callback runs on SDL's own audio thread, created inside
 * SDL_OpenAudioDeviceStream() — which we call from the MAIN thread while it still has
 * SIGALRM/SIGTERM/SIGUSR1 blocked (main.c blocks them before spawning the game thread). The audio
 * thread inherits that mask, so the game-thread tick is never delivered to it. The callback is the
 * single consumer of both rings (audio.c only READS ->r/->w for the pacer; it never writes ->r), so
 * touching ->r / ->midi->r from the callback is race-free — the same single-producer/single-consumer
 * lock-free discipline the two-process host used. All consumer globals are published BEFORE the
 * audio thread exists (we set them, then open the stream), so the callback never sees them half-set.
 *
 * Guarded #ifdef ROTH_STANDALONE + linked only into the moddable engine binary (roth); SDL symbols are SDL_* so the
 * Makefile nm-clean assertion is unaffected.
 */
#ifdef ROTH_STANDALONE

#include <SDL3/SDL.h>

#include "sdl_audio.h"
#include "../shared_audio.h"
#include "../shared_midi.h"
#include "../viewer/tsf.h"   /* NO TSF_IMPLEMENTATION here: extern API prototypes only (impl in sdl_tsf.c) */
#include "../sys/sys.h"      /* per-OS seam: directory enumeration for the system-SoundFont scan */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp (system-soundfont extension match) */
#include <sys/mman.h>
#include <unistd.h>

/* our own second views of the producer's shm mailboxes (NOT audio.c's static g_au/g_midi). The
 * get-callback (SDL audio thread) is the sole writer of ->r on both; audio.c is the sole writer of
 * ->w. Published before the audio thread is created, so no volatile/atomic hand-off is needed. */
static struct roth_audio *g_au;
static struct roth_midi  *g_midi;
static tsf               *g_tsf;             /* SoundFont synth for the MIDI music */
static SDL_AudioStream   *g_stream;
static int                g_bytes_per_frame = 4;
static int                g_audio_ready;     /* stream open + resumed — stop polling */
static int                g_audio_gaveup;    /* a hard failure (init/open) — stop polling */
static int                g_audio_inited;    /* SDL_InitSubSystem(AUDIO) succeeded */
static volatile unsigned long g_cb_calls;    /* callback invocation count (consumer-clock proof) */

/* M4 zero-config path inputs (defined in main.c under ROTH_STANDALONE): --sf2 override + the running
 * binary's own directory, for the sf2-beside-exe lookup. */
extern const char *g_exe_dir;
extern const char *g_sf2_path;

/* Producer-side backing selection + ring pointers (audio.c). When the in-process backing is active we
 * read these pointers directly instead of mapping a second view of a named shared-memory object. */
extern int                audio_backing_in_process(void);
extern struct roth_audio *audio_pcm_ring(void);
extern struct roth_midi  *audio_midi_ring(void);

/* M4 SoundFont lookup order (design §5 + director addendum): (1) --sf2, (2) ROTH_SF2 env, (3) gm.sf2
 * beside the executable (the product case), (4) the legacy dev default, (5) the Linux system default
 * /usr/share/soundfonts/default.sf2, (6) the first *.sf2 in /usr/share/sounds/sf2/. Returns the chosen
 * path (into `buf` for computed cases, or a literal), or NULL if none — caller goes MIDI-silent. */
/* Collector for the system-SoundFont scan: keep the first *.sf2 entry (case-insensitive). */
struct sf2_pick_ctx {
    char       *buf;
    size_t      bufsz;
    const char *pick;
};

static int sf2_pick_first(const char *name, void *ud)
{
    struct sf2_pick_ctx *c = ud;
    size_t l = strlen(name);
    if (l > 4 && !strcasecmp(name + l - 4, ".sf2")) {
        snprintf(c->buf, c->bufsz, "/usr/share/sounds/sf2/%s", name);
        c->pick = c->buf;
        return 0;                                                   /* first match wins — stop */
    }
    return 1;
}

static const char *resolve_sf2(char *buf, size_t bufsz)
{
    if (g_sf2_path && *g_sf2_path)
        return g_sf2_path;                                          /* 1. --sf2 */
    const char *env = getenv("ROTH_SF2");
    if (env && *env)
        return env;                                                 /* 2. ROTH_SF2 */
    if (g_exe_dir) {                                                /* 3. gm.sf2 beside the exe */
        snprintf(buf, bufsz, "%s/gm.sf2", g_exe_dir);
        if (access(buf, R_OK) == 0)
            return buf;
    }
    if (access("recomp/viewer/gm.sf2", R_OK) == 0)                  /* 4. legacy dev default (CWD) */
        return "recomp/viewer/gm.sf2";
    if (access("/usr/share/soundfonts/default.sf2", R_OK) == 0)     /* 5. system default */
        return "/usr/share/soundfonts/default.sf2";
    struct sf2_pick_ctx c = { buf, bufsz, NULL };                   /* 6. first *.sf2 in the sf2 dir */
    sys_enum_dir("/usr/share/sounds/sf2", sf2_pick_first, &c);
    if (c.pick)
        return c.pick;
    return NULL;                                                    /* 7. none found */
}

/* The SDL audio get-callback: SDL asks for `additional_amount` bytes (in our SOURCE spec —
 * S16LE/2ch/22050 by default) just before feeding the device. We generate them by draining the PCM
 * ring and mixing the MIDI synth over it, exactly as viewer.c:94-136, then hand them to SDL with
 * SDL_PutAudioStreamData (SDL resamples/converts to the device format). Runs on the SDL audio
 * thread. `static buf` is safe: SDL serialises calls to a stream's callback (single audio thread). */
static void SDLCALL audio_get_cb(void *ud, SDL_AudioStream *stream,
                                 int additional_amount, int total_amount)
{
    (void)ud;
    (void)total_amount;
    struct roth_audio *au = g_au;
    if (!au)
        return; /* not mapped yet (shouldn't happen: we open the stream only after mapping) */

    g_cb_calls++;

    static uint8_t buf[8192];   /* frame-aligned scratch (2048 S16 stereo frames) */
    const uint8_t silence = (au->bits == 8) ? 0x80 : 0x00; /* centered, matches viewer.c:97 */

    while (additional_amount > 0) {
        int chunk = additional_amount < (int)sizeof buf ? additional_amount : (int)sizeof buf;

        /* drain the PCM ring (viewer.c:102-108); ->r is THE consumer clock the movie pacer reads. */
        uint32_t avail = au->w - au->r;
        uint32_t n = (uint32_t)chunk < avail ? (uint32_t)chunk : avail;
        for (uint32_t i = 0; i < n; i++)
            buf[i] = au->ring[(au->r + i) & ROTH_AUDIO_MASK];
        au->r += n;                                   /* advance the consumer clock (verbatim viewer) */
        if (n < (uint32_t)chunk)
            memset(buf + n, silence, (uint32_t)chunk - n); /* underrun -> silence, no spam */

        /* MIDI music: drain the event ring into the synth and mix over the PCM (16-bit stereo only;
         * viewer.c:112-135). Front-loaded per chunk exactly as the viewer front-loads per callback. */
        if (g_tsf && g_midi && au->bits == 16 && au->channels == 2) {
            uint32_t mw = g_midi->w;
            while (g_midi->r != mw) {
                struct roth_midi_ev e = g_midi->ev[g_midi->r & ROTH_MIDI_MASK];
                g_midi->r++;
                int ch = e.status & 0x0f, cmd = e.status & 0xf0;
                switch (cmd) {
                case 0x90:
                    if (e.d2)
                        tsf_channel_note_on(g_tsf, ch, e.d1, e.d2 / 127.0f);
                    else
                        tsf_channel_note_off(g_tsf, ch, e.d1);
                    break;
                case 0x80: tsf_channel_note_off(g_tsf, ch, e.d1); break;
                case 0xb0: tsf_channel_midi_control(g_tsf, ch, e.d1, e.d2); break;
                case 0xc0: tsf_channel_set_presetnumber(g_tsf, ch, e.d1, ch == 9); break;
                case 0xe0: tsf_channel_set_pitchwheel(g_tsf, ch, e.d1 | (e.d2 << 7)); break;
                default: break; /* aftertouch etc. — ignore, as the viewer does */
                }
            }
            tsf_render_short(g_tsf, (short *)buf, chunk / 4, 1 /* mix into PCM */);
        }

        SDL_PutAudioStreamData(stream, buf, chunk);
        additional_amount -= chunk;
    }
}

void sdl_audio_poll_open(void)
{
    if (g_audio_ready || g_audio_gaveup)
        return;

    /* Two backings for the producer's rings (see the file header). In-process: read the producer's own
     * pointers directly. Shared: map the named object a second time. Both are retriable — audio_init()
     * runs on the game thread and may not have set the ring up yet when the present loop first spins. */
    int in_proc = audio_backing_in_process();
    struct roth_audio *au;

    if (in_proc) {
        au = audio_pcm_ring();  /* the producer's own buffer — no second view to map */
        if (!au || au->magic != ROTH_AUDIO_MAGIC || !au->ready)
            return;             /* not set up yet -> try again next present frame */
    } else {
        int fd = shm_open(ROTH_AUDIO_SHM_NAME, O_RDWR, 0600);
        if (fd < 0)
            return;                 /* not created yet -> try again next present frame */
        au = mmap(NULL, sizeof *au, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (au == MAP_FAILED)
            return;
        if (au->magic != ROTH_AUDIO_MAGIC || !au->ready) {
            munmap(au, sizeof *au); /* created but not initialized yet -> retry next frame */
            return;
        }
    }

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {   /* SDL3: bool return, true = ok */
        fprintf(stderr, "[sdl-audio] SDL_InitSubSystem(AUDIO): %s — no sound\n", SDL_GetError());
        if (!in_proc)
            munmap(au, sizeof *au); /* in-process: the ring is the producer's — never unmap it */
        g_audio_gaveup = 1;
        return;
    }
    g_audio_inited = 1;

    /* MIDI ring is OPTIONAL: absent when ROTH_MIDI=0 (audio.c never creates the ring then). In-process,
     * take the producer's pointer directly; shared, map the named object. If it's absent/not ready or the
     * SoundFont fails to load, MIDI is silent and PCM still plays. */
    struct roth_midi *midi = NULL;
    if (in_proc) {
        midi = audio_midi_ring();
        if (midi && (midi->magic != ROTH_MIDI_MAGIC || !midi->ready))
            midi = NULL;
    } else {
        int mfd = shm_open(ROTH_MIDI_SHM_NAME, O_RDWR, 0600);
        if (mfd >= 0) {
            midi = mmap(NULL, sizeof *midi, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
            close(mfd);
            if (midi == MAP_FAILED || midi->magic != ROTH_MIDI_MAGIC || !midi->ready)
                midi = NULL;
        }
    }
    tsf *synth = NULL;
    if (midi) {
        char sfbuf[1024];
        const char *sf = resolve_sf2(sfbuf, sizeof sfbuf);   /* M4 lookup chain (design §5) */
        if (sf)
            synth = tsf_load_filename(sf);
        if (synth) {
            tsf_channel_set_presetnumber(synth, 9, 0, 1);                 /* ch10 = GM drums */
            tsf_set_output(synth, TSF_STEREO_INTERLEAVED, (int)au->rate, 0.0f);
            fprintf(stderr, "[sdl-audio] MIDI music on (SoundFont %s)\n", sf);
        } else if (sf) {
            fprintf(stderr, "[sdl-audio] SoundFont '%s' failed to load — no music "
                            "(set ROTH_SF2)\n", sf);
        } else {
            fprintf(stderr, "[sdl-audio] no SoundFont found — no music "
                            "(set ROTH_SF2 or put gm.sf2 beside the binary)\n");
        }
    }

    /* Publish the consumer state BEFORE the audio thread exists (created inside
     * SDL_OpenAudioDeviceStream), so the first get-callback sees fully-initialized globals. */
    g_bytes_per_frame = (int)((au->bits / 8) * au->channels);
    if (g_bytes_per_frame < 1)
        g_bytes_per_frame = 1;
    g_midi = midi;
    g_tsf = synth;
    g_au = au;

    /* SDL3 audio-stream (get-callback model): request the producer's native format as our SOURCE
     * spec; SDL auto-resamples/converts to whatever the device wants, so the SDL2 want/have
     * negotiation (viewer.c:149-160) disappears (design §12). Format mirrors the producer exactly:
     * viewer.c:150-152 read the same rate/bits/channels from the same struct. */
    SDL_AudioSpec spec;
    spec.format = (au->bits == 16) ? SDL_AUDIO_S16LE : SDL_AUDIO_U8;
    spec.channels = (int)au->channels;
    spec.freq = (int)au->rate;
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                         audio_get_cb, NULL);
    if (!g_stream) {
        fprintf(stderr, "[sdl-audio] SDL_OpenAudioDeviceStream: %s — no sound\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_inited = 0;
        g_audio_gaveup = 1;     /* leave g_au mapped-but-idle; the host falls back to the free-running
                                 * movie pacer (no consumer draining ->r), exactly like headless */
        return;
    }
    SDL_ResumeAudioStreamDevice(g_stream);   /* streams open paused */
    fprintf(stderr, "[sdl-audio] audio %u Hz, %u-bit, %u ch (in-process SDL3 stream)\n",
            au->rate, au->bits, au->channels);
    g_audio_ready = 1;
}

void sdl_audio_log_tick(void)
{
    static int en = -1;
    if (en < 0)
        en = getenv("ROTH_SDL_AUDIO_LOG") ? 1 : 0;
    if (!en || !g_au)
        return;
    static int div;
    static uint32_t last_r;
    if (++div < 120)            /* ~ every 2 s at 60 fps present cadence */
        return;
    div = 0;
    fprintf(stderr, "[sdl-audio] cb=%lu r=%u w=%u (consumed since last=%u)\n",
            g_cb_calls, g_au->r, g_au->w, g_au->r - last_r);
    last_r = g_au->r;
}

void sdl_audio_shutdown(void)
{
    if (g_stream) {
        SDL_DestroyAudioStream(g_stream); /* stops + joins the callback and closes the device */
        g_stream = NULL;
    }
    /* the audio thread is gone now, so no callback can be touching g_tsf/g_au anymore */
    if (g_tsf) {
        tsf_close(g_tsf);
        g_tsf = NULL;
    }
    g_au = NULL;
    g_midi = NULL;
    if (g_audio_inited) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_inited = 0;
    }
}

#endif /* ROTH_STANDALONE */
