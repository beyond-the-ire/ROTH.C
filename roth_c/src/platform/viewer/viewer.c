/* roth-view — standalone SDL2 window for the ROTH host.
 *
 * Mmaps the shared framebuffer the 32-bit host publishes, blits the mode-13h
 * image through the captured VGA palette, and feeds keyboard/mouse back to the
 * host. This process can be any arch (uses the system SDL2); the game host
 * stays a pure 32-bit process. See host/shared_fb.h for the contract.
 *
 * Build: make -C recomp/viewer
 * Run:   recomp/viewer/roth-view [scale]
 */
#include "../shared_fb.h"
#include "../shared_audio.h"
#include "../shared_midi.h"

#define TSF_IMPLEMENTATION
#include "tsf.h"

#include <SDL2/SDL.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* SDL_Scancode -> PC set-1 make code. 0 = unmapped. Extended keys carry a
 * 0xE0 prefix flag in the high byte (0x100). */
#define EXT 0x100
static int sdl_to_set1(SDL_Scancode s)
{
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03;
    case SDL_SCANCODE_3: return 0x04; case SDL_SCANCODE_4: return 0x05;
    case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09;
    case SDL_SCANCODE_9: return 0x0a; case SDL_SCANCODE_0: return 0x0b;
    case SDL_SCANCODE_MINUS: return 0x0c; case SDL_SCANCODE_EQUALS: return 0x0d;
    case SDL_SCANCODE_BACKSPACE: return 0x0e; case SDL_SCANCODE_TAB: return 0x0f;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11;
    case SDL_SCANCODE_E: return 0x12; case SDL_SCANCODE_R: return 0x13;
    case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17;
    case SDL_SCANCODE_O: return 0x18; case SDL_SCANCODE_P: return 0x19;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_RETURN: return 0x1c;
    case SDL_SCANCODE_LCTRL: return 0x1d;
    case SDL_SCANCODE_A: return 0x1e; case SDL_SCANCODE_S: return 0x1f;
    case SDL_SCANCODE_D: return 0x20; case SDL_SCANCODE_F: return 0x21;
    case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25;
    case SDL_SCANCODE_L: return 0x26; case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2a; case SDL_SCANCODE_BACKSLASH: return 0x2b;
    case SDL_SCANCODE_Z: return 0x2c; case SDL_SCANCODE_X: return 0x2d;
    case SDL_SCANCODE_C: return 0x2e; case SDL_SCANCODE_V: return 0x2f;
    case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3a;
    case SDL_SCANCODE_F1: return 0x3b; case SDL_SCANCODE_F2: return 0x3c;
    case SDL_SCANCODE_F3: return 0x3d; case SDL_SCANCODE_F4: return 0x3e;
    case SDL_SCANCODE_F5: return 0x3f; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42;
    case SDL_SCANCODE_F9: return 0x43; case SDL_SCANCODE_F10: return 0x44;
    case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    /* extended (0xE0-prefixed) keys */
    case SDL_SCANCODE_UP: return EXT | 0x48;
    case SDL_SCANCODE_DOWN: return EXT | 0x50;
    case SDL_SCANCODE_LEFT: return EXT | 0x4b;
    case SDL_SCANCODE_RIGHT: return EXT | 0x4d;
    case SDL_SCANCODE_RCTRL: return EXT | 0x1d;
    case SDL_SCANCODE_RALT: return EXT | 0x38;
    case SDL_SCANCODE_HOME: return EXT | 0x47;
    case SDL_SCANCODE_END: return EXT | 0x4f;
    case SDL_SCANCODE_PAGEUP: return EXT | 0x49;
    case SDL_SCANCODE_PAGEDOWN: return EXT | 0x51;
    case SDL_SCANCODE_INSERT: return EXT | 0x52;
    case SDL_SCANCODE_DELETE: return EXT | 0x53;
    case SDL_SCANCODE_KP_ENTER: return EXT | 0x1c;
    default: return 0;
    }
}

static struct roth_shm *g_shm;

/* ---- audio: drain the host's PCM ring into the SDL audio device ---------- */
static struct roth_audio *g_au;
static struct roth_midi *g_midi; /* MIDI-event ring from the host */
static tsf *g_tsf;               /* SoundFont synth for the MIDI music */

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    uint8_t silence = (g_au && g_au->bits == 8) ? 0x80 : 0x00; /* centered */
    if (!g_au) {
        memset(stream, silence, len);
        return;
    }
    uint32_t avail = g_au->w - g_au->r;
    uint32_t n = (uint32_t)len < avail ? (uint32_t)len : avail;
    for (uint32_t i = 0; i < n; i++)
        stream[i] = g_au->ring[(g_au->r + i) & ROTH_AUDIO_MASK];
    g_au->r += n;
    if (n < (uint32_t)len)
        memset(stream + n, silence, (uint32_t)len - n); /* underrun: silence */

    /* MIDI music: drain the event ring into the SoundFont synth and mix its
     * output over the digital PCM (16-bit stereo only). */
    if (g_tsf && g_midi && g_au->bits == 16 && g_au->channels == 2) {
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
            case 0xe0:
                tsf_channel_set_pitchwheel(g_tsf, ch, e.d1 | (e.d2 << 7));
                break;
            default: break; /* aftertouch etc. — ignore */
            }
        }
        tsf_render_short(g_tsf, (short *)stream, len / 4, 1 /* mix into PCM */);
    }
}

static void audio_open(void)
{
    int fd = shm_open(ROTH_AUDIO_SHM_NAME, O_RDWR, 0600);
    if (fd < 0)
        return; /* host built without audio / not ready: silent, no error */
    g_au = mmap(NULL, sizeof *g_au, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_au == MAP_FAILED || g_au->magic != ROTH_AUDIO_MAGIC || !g_au->ready) {
        g_au = NULL;
        return;
    }
    SDL_AudioSpec want = {0}, have;
    want.freq = (int)g_au->rate;
    want.format = (g_au->bits == 16) ? AUDIO_S16LSB : AUDIO_U8;
    want.channels = (Uint8)g_au->channels;
    want.samples = 1024;
    want.callback = audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        fprintf(stderr, "roth-view: SDL_OpenAudioDevice: %s\n", SDL_GetError());
        g_au = NULL;
        return;
    }
    fprintf(stderr, "roth-view: audio %u Hz, %u-bit, %u ch\n", g_au->rate,
            g_au->bits, g_au->channels);

    /* MIDI music: map the host's MIDI-event ring and load the SoundFont synth. */
    int mfd = shm_open(ROTH_MIDI_SHM_NAME, O_RDWR, 0600);
    if (mfd >= 0) {
        g_midi = mmap(NULL, sizeof *g_midi, PROT_READ | PROT_WRITE, MAP_SHARED,
                      mfd, 0);
        close(mfd);
        if (g_midi == MAP_FAILED || g_midi->magic != ROTH_MIDI_MAGIC)
            g_midi = NULL;
    }
    if (g_midi) {
        const char *sf = getenv("ROTH_SF2");
        if (!sf || !*sf)
            sf = "recomp/viewer/gm.sf2";
        g_tsf = tsf_load_filename(sf);
        if (g_tsf) {
            tsf_channel_set_presetnumber(g_tsf, 9, 0, 1); /* ch10 = GM drums */
            tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, (int)g_au->rate, 0.0f);
            fprintf(stderr, "roth-view: MIDI music on (SoundFont %s)\n", sf);
        } else {
            fprintf(stderr, "roth-view: SoundFont '%s' failed to load — no "
                            "music (set ROTH_SF2)\n", sf);
        }
    }
    SDL_PauseAudioDevice(dev, 0);
}

static void push_key(uint8_t code)
{
    g_shm->key_ring[g_shm->key_head & ROTH_KEY_MASK] = code;
    g_shm->key_head++;
}

static void send_key(SDL_Scancode s, int down)
{
    int code = sdl_to_set1(s);
    if (!code)
        return;
    if (code & EXT)
        push_key(0xe0);
    uint8_t make = code & 0xff;
    push_key(down ? make : (make | 0x80));
}

int main(int argc, char **argv)
{
    int scale = argc > 1 ? atoi(argv[1]) : 0; /* 0 = auto-fit the display */

    int fd = shm_open(ROTH_SHM_NAME, O_RDWR, 0600);
    if (fd < 0) {
        fprintf(stderr, "roth-view: host not running (no %s). "
                "Start roth-host first.\n", ROTH_SHM_NAME);
        return 1;
    }
    g_shm = mmap(NULL, sizeof *g_shm, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_shm == MAP_FAILED || g_shm->magic != ROTH_SHM_MAGIC) {
        fprintf(stderr, "roth-view: shared memory not initialized yet.\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    audio_open(); /* optional: silent if the host has no audio ring */
    /* Auto-pick a window scale that fills ~85% of the display height, so it's
     * comfortably large on hi-DPI screens; ROTH_SCALE / argv override it. */
    if (scale < 1) {
        SDL_DisplayMode dm;
        scale = 4;
        if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
            int s = (int)(dm.h * 0.85 / ROTH_FB_H);
            scale = s < 2 ? 2 : s;
        }
    }
    /* crisp integer upscaling (nearest-neighbour), not blurry */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *win = SDL_CreateWindow(
        "Realms of the Haunting (native host)", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, ROTH_FB_W * scale, ROTH_FB_H * scale,
        SDL_WINDOW_RESIZABLE); /* drag/maximize to any size; content scales */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, ROTH_FB_W, ROTH_FB_H);
    SDL_RenderSetIntegerScale(ren, SDL_FALSE); /* allow filling the window */
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         ROTH_FB_W, ROTH_FB_H);
    int cur_tex_w = ROTH_FB_W, cur_tex_h = ROTH_FB_H;
    int cur_log_w = ROTH_FB_W, cur_log_h = ROTH_FB_H;
    g_shm->viewer_alive = 1;

    /* Capture the pointer: the in-game cursor is driven by relative motion
     * (int33 AX=000B), so we hide+confine the OS cursor and feed raw deltas.
     * The pointer is released when the window loses focus, and re-grabbed on
     * click. Sensitivity scales physical motion to in-game cursor speed. */
    const char *se = getenv("ROTH_MOUSE_SENS");
    double sens = se ? atof(se) : 2.0;
    if (sens <= 0) sens = 2.0;
    SDL_SetRelativeMouseMode(SDL_TRUE);
    int grabbed = 1;
    double ax = 0, ay = 0; /* fractional delta accumulators */

    static uint32_t rgb[ROTH_FB_MAX]; /* fits hi-res (640x480) */
    int running = 1;
    while (running && g_shm->host_alive) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                running = 0;
            else if (e.type == SDL_KEYDOWN && !e.key.repeat)
                send_key(e.key.keysym.scancode, 1);
            else if (e.type == SDL_KEYUP)
                send_key(e.key.keysym.scancode, 0);
            else if (e.type == SDL_MOUSEMOTION && grabbed) {
                /* scaled relative deltas drive the in-game cursor (AX=000B) */
                ax += e.motion.xrel * sens;
                ay += e.motion.yrel * sens;
                int idx = (int)ax, idy = (int)ay; /* keep the fraction */
                ax -= idx; ay -= idy;
                g_shm->mouse_dx += idx;
                g_shm->mouse_dy += idy;
            } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                if (!grabbed && e.type == SDL_MOUSEBUTTONDOWN) {
                    SDL_SetRelativeMouseMode(SDL_TRUE); /* click to re-grab */
                    grabbed = 1;
                }
                uint32_t b = 0;
                uint32_t st = SDL_GetMouseState(NULL, NULL);
                if (st & SDL_BUTTON(SDL_BUTTON_LEFT)) b |= 1;
                if (st & SDL_BUTTON(SDL_BUTTON_RIGHT)) b |= 2;
                if (st & SDL_BUTTON(SDL_BUTTON_MIDDLE)) b |= 4;
                g_shm->mouse_buttons = b;
            } else if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    SDL_SetRelativeMouseMode(SDL_FALSE); /* free cursor on alt-tab */
                    grabbed = 0;
                }
            }
        }

        /* follow the host's current resolution + intended display aspect. The
         * pixel buffer is w x h, but the display aspect (aw:ah) may differ:
         * line-doubled 320x400 publishes aw:ah = 320:200 so it fills a 4:3-ish
         * screen instead of a tall portrait. The texture stays w x h (full
         * detail); RenderSetLogicalSize scales it to the aspect rectangle. */
        int w = (int)g_shm->cur_w, h = (int)g_shm->cur_h;
        if (w <= 0 || w > ROTH_FB_MAXW) w = ROTH_FB_W;
        if (h <= 0 || h > ROTH_FB_MAXH) h = ROTH_FB_H;
        int aw = (int)g_shm->aspect_w, ah = (int)g_shm->aspect_h;
        if (aw <= 0 || aw > ROTH_FB_MAXW || ah <= 0 || ah > ROTH_FB_MAXH) {
            aw = w; ah = h; /* fallback: square pixels */
        }
        if (w != cur_tex_w || h != cur_tex_h) {
            SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, w, h);
            cur_tex_w = w; cur_tex_h = h;
        }
        if (aw != cur_log_w || ah != cur_log_h) {
            SDL_RenderSetLogicalSize(ren, aw, ah);
            cur_log_w = aw; cur_log_h = ah;
        }
        int n = w * h;
        for (int i = 0; i < n; i++) {
            uint8_t idx = g_shm->pixels[i];
            uint8_t r = g_shm->palette[idx * 3 + 0] << 2;
            uint8_t g = g_shm->palette[idx * 3 + 1] << 2;
            uint8_t b = g_shm->palette[idx * 3 + 2] << 2;
            rgb[i] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
        SDL_UpdateTexture(tex, NULL, rgb, w * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren); /* vsync paces the loop */
    }

    g_shm->viewer_alive = 0;
    if (!running)
        g_shm->quit = 1; /* user closed the window: ask host to exit */
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
