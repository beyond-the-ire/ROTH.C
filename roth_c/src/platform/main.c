/* ROTH native host — loader and entry.
 *
 * Maps the relocated LE objects at their original VAs, fabricates a minimal
 * DOS environment (PSP + env block, LDT selectors), and jumps to the LE entry
 * with the original initial ESP. All OS interaction traps into traps.c.
 *
 * Build: make -C roth_c
 * Run:   recomp/host/roth-host [--game-dir DIR] [--trace] [obj1.bin obj2.bin obj3.bin]
 */
#include "roth_host.h"
#include "shared_fb.h"
#include "audio.h"
#include "audio_trace.h"
#include "lift_registry.h"
#include "calltrace.h"
#include "capture.h"

#include <asm/ldt.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef ROTH_STANDALONE
/* M1 SDL fold-in (task #102 / docs/SDL3_FOLD_DESIGN.md): the in-process SDL window + the
 * game-on-a-child-thread arrangement. Guarded so the trap host (compiles main.c WITHOUT
 * ROTH_STANDALONE) never sees pthread/SDL — its link stays -lrt-only and behaviour-identical. */
#include <pthread.h>
#include <signal.h>
#include <dirent.h>    /* M4: case-insensitive CONFIG.INI probe for the exe-dir game-dir default */
#include <strings.h>   /* strcasecmp */
#include "sdl/sdl_host.h"
#include "plugin_loader.h"   /* task #103: the runtime plugin platform (imgfree only) */
#endif

sigjmp_buf g_exit_jmp;
int g_exit_code = -1;
const char *g_game_dir = ".";
int g_trace = 0;

static uint16_t g_host_es, g_host_ds;
uint16_t g_host_fs, g_host_gs;
struct roth_shm *g_shm;

void shm_setup(void)
{
#ifdef ROTH_STANDALONE
    /* Idempotent: M1's --sdl path creates the shm backing on the main thread BEFORE the game
     * thread starts (design §3/§4 "main picks the backing"), so roth_boot()'s own shm_setup() call
     * on the game thread becomes a no-op. Guarded so the trap host's shm_setup() (called once) is
     * byte-identical to before. */
    if (g_shm)
        return;
#endif
    int fd = shm_open(ROTH_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        LOGE("shm_open failed: %s (running headless)\n", strerror(errno));
        return;
    }
    if (ftruncate(fd, sizeof(struct roth_shm)) != 0) {
        LOGE("ftruncate failed: %s\n", strerror(errno));
        close(fd);
        return;
    }
    g_shm = mmap(NULL, sizeof(struct roth_shm), PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);
    close(fd);
    if (g_shm == MAP_FAILED) {
        g_shm = NULL;
        LOGE("shm mmap failed (running headless)\n");
        return;
    }
    memset(g_shm, 0, sizeof *g_shm);
    g_shm->magic = ROTH_SHM_MAGIC;
    g_shm->width = ROTH_FB_W;
    g_shm->height = ROTH_FB_H;
    g_shm->host_alive = 1;
    LOGE("shared framebuffer ready at " ROTH_SHM_NAME
         " — run recomp/viewer/roth-view\n");
}

#ifdef ROTH_STANDALONE
/* --------- SDL fold-in: flags + the game thread (docs/SDL3_FOLD_DESIGN.md §3, "Model A") -------- */
/* M3 default-flip (design §6): the SDL window is the DEFAULT experience. --headless / ROTH_HEADLESS=1
 * selects today's no-window shm path (the test rigs' lane) — kept FIRST-CLASS + byte-identical.
 * --sdl / --windowed are accepted no-op aliases (the window is already the default). --headless --sdl
 * (or ROTH_SHM=1 under a window) publishes /dev/shm/roth_fb alongside the window ("both" mode). */
static int g_headless  = 0;    /* --headless / ROTH_HEADLESS=1: no window, shm published (rigs/CI) */
static int g_sdl_flag  = 0;    /* --sdl / --windowed / ROTH_SDL=1: explicit window (both-mode w/ --headless) */
static int g_want_shm  = 0;    /* ROTH_SHM=1: publish /dev/shm/roth_fb even under a window */
static int g_sdl_scale = 0;    /* --scale N / ROTH_SCALE: initial window scale (0 = auto-fit) */
/* task #103 plugin platform (safe mode / listing; finding 18). MODDABLE flavor only — the vanilla
 * flavor (-DROTH_VANILLA) links no plugin loader, so these options don't exist there (§12). */
#ifndef ROTH_VANILLA
static int g_no_mods     = 0;  /* --no-mods: disable the plugin platform (ROTH_MODS=0 alias) */
static int g_strict_mods = 0;  /* --strict-mods: abort the boot on any plugin validation failure */
static int g_list_mods   = 0;  /* --list-mods: discover+validate+print the resolved report, then exit */
static int g_dump_chains = 0;  /* --dump-mod-chains: print the resolved override chains at boot (finding 13) */
#endif

/* M4 zero-config path resolution (design §5). Set in main() from /proc/self/exe. */
const char *g_exe_dir  = NULL; /* dir of the running binary — sf2-beside-exe lookup (sdl_audio.c) */
const char *g_sf2_path = NULL; /* --sf2 override; NULL => ROTH_SF2 / beside-exe / system (sdl_audio.c) */

/* Dev-convenience fallback: the ROTH_DEV_GAME_DIR environment variable may name a game install to
 * try when the content isn't beside the binary and --game-dir wasn't given. Developers export it;
 * end users never need it (the product flow is content-beside-the-binary or --game-dir). */

/* M3 windowed default: a PRIVATE in-process framebuffer backing (no /dev/shm file). The SDL present
 * loop reads it in the same address space; nothing external attaches. --headless / ROTH_SHM=1 keep
 * the shm_open backing (shm_setup) instead. Idempotent like shm_setup, so roth_boot()'s own
 * shm_setup() no-ops. (Audio's /roth_audio + /roth_midi are still shm_open'd by the untouched audio
 * producer; only the framebuffer is demoted to private here — design §4/§6.) */
static void mem_setup(void)
{
    if (g_shm)
        return;
    g_shm = mmap(NULL, sizeof(struct roth_shm), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_shm == MAP_FAILED) {
        g_shm = NULL;
        LOGE("private framebuffer mmap failed: %s\n", strerror(errno));
        return;
    }
    memset(g_shm, 0, sizeof *g_shm);
    g_shm->magic = ROTH_SHM_MAGIC;
    g_shm->width = ROTH_FB_W;
    g_shm->height = ROTH_FB_H;
    g_shm->host_alive = 1;
}

/* Case-insensitive test for CONFIG.INI (the game's config file, the game-dir sentinel) under `dir`. */
static int dir_has_config_ini(const char *dir)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/CONFIG.INI", dir);
    if (access(p, F_OK) == 0)
        return 1;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)))
        if (!strcasecmp(e->d_name, "CONFIG.INI")) { found = 1; break; }
    closedir(d);
    return found;
}

/* M4 zero-config (design §5): resolve g_exe_dir (readlink /proc/self/exe -> dirname; argv[0] fallback),
 * and — unless --game-dir was given — default the game-dir to the executable's OWN directory so
 * dropping the binary into the ROTH content folder and running it just works. If CONFIG.INI isn't
 * beside the exe, fall back to the dev Steam path; if that's absent too, fail clearly naming both. */
static void resolve_paths(const char *argv0, int game_dir_given)
{
    static char exedir[1024];
    ssize_t n = readlink("/proc/self/exe", exedir, sizeof exedir - 1);
    if (n > 0)
        exedir[n] = 0;
    else
        snprintf(exedir, sizeof exedir, "%s", argv0 ? argv0 : ".");
    char *slash = strrchr(exedir, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(exedir, ".");
    g_exe_dir = exedir;

    if (game_dir_given)
        return; /* keep the explicit --game-dir; g_exe_dir is still set for the sf2 lookup */

    if (dir_has_config_ini(exedir)) {
        g_game_dir = exedir;                 /* content beside the binary — the product case */
        return;
    }
    const char *devdir = getenv("ROTH_DEV_GAME_DIR");
    if (devdir && dir_has_config_ini(devdir)) {
        g_game_dir = devdir;                 /* dev convenience (env-provided) */
        LOGE("game content not beside the binary; using ROTH_DEV_GAME_DIR %s\n", devdir);
        return;
    }
    LOGE("cannot find CONFIG.INI beside the executable (%s).\n"
         "   Put the ROTH game files (CONFIG.INI, ROTH.RES, DBASE*.DAT, *.DAS) beside the binary,\n"
         "   or pass --game-dir DIR (or export ROTH_DEV_GAME_DIR).\n", exedir);
    exit(1);
}

/* The game thread: owns the whole game + its SIGALRM ISR-surrogate tick (shm_tick), which must
 * PREEMPT this thread cooperatively exactly as it does on the main thread today. main() blocked
 * SIGALRM/SIGTERM/SIGUSR1 before creating us (threads inherit the mask); we unblock them HERE so
 * the process-directed ITIMER_REAL — and the SIGTERM screenshot / SIGUSR1 probe, whose handlers
 * touch fs/gs — are delivered to THIS thread only. With just this thread unblocked, the tick keeps
 * being a single-threaded preemption of game code: zero new races on the ISR-shared globals. */
static void *game_main(void *arg)
{
    (void)arg;
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGALRM);
    sigaddset(&s, SIGTERM);
    sigaddset(&s, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    /* standalone runs FLAT (no game LDT selectors loaded), so fs/gs during game execution are this
     * thread's glibc TLS. The SIGALRM/SIGTERM handlers swap fs/gs to g_host_fs/g_host_gs — which
     * must therefore be THIS thread's TLS (making the swap a true no-op), NOT main()'s TLS, or libc
     * called from the handler would read the wrong thread's TLS. Re-capture them on the game thread. */
    __asm__ volatile("mov %%fs, %0" : "=r"(g_host_fs));
    __asm__ volatile("mov %%gs, %0" : "=r"(g_host_gs));
    roth_boot();
    LOGE("game_main: roth_boot returned — game exited (code %d)\n", g_exit_code);
    sfx_trace_exit_dump();   /* SFX-dropout trace: windowed clean-quit flush (game thread, quiesced) */
    if (g_shm)
        g_shm->host_alive = 0;   /* signal the SDL present loop on the main thread to stop */
    return NULL;
}
#endif /* ROTH_STANDALONE */

/* ---------------------------------------------------------------- LDT -- */

static int modify_ldt_write(struct user_desc *d)
{
    return (int)syscall(SYS_modify_ldt, 0x11, d, sizeof *d);
}

static int g_ldt_used[8192];

static int ldt_install(int entry, uint32_t base, uint32_t limit_bytes)
{
    struct user_desc d = {0};
    d.entry_number = entry;
    d.base_addr = base;
    /* DPMI limits are byte-granular below 1M; use page granularity above. */
    if (limit_bytes >= 0x100000) {
        d.limit = limit_bytes >> 12;
        d.limit_in_pages = 1;
    } else {
        d.limit = limit_bytes;
        d.limit_in_pages = 0;
    }
    d.seg_32bit = 1;
    d.contents = MODIFY_LDT_CONTENTS_DATA;
    d.useable = 1;
    return modify_ldt_write(&d);
}

int ldt_alloc(uint32_t base, uint32_t limit_bytes)
{
    for (int i = 1; i < 8192; i++) { /* skip entry 0: keep null-ish low */
        if (!g_ldt_used[i]) {
            if (ldt_install(i, base, limit_bytes) != 0)
                return -1;
            g_ldt_used[i] = 1;
            return (i << 3) | 0x7; /* LDT, RPL3 */
        }
    }
    return -1;
}

static uint32_t g_sel_base[8192], g_sel_limit[8192];

int ldt_set_base(uint16_t sel, uint32_t base)
{
    int e = sel >> 3;
    g_sel_base[e] = base;
    return ldt_install(e, base, g_sel_limit[e] ? g_sel_limit[e] : 0xfffff);
}

int ldt_set_limit(uint16_t sel, uint32_t limit_bytes)
{
    int e = sel >> 3;
    g_sel_limit[e] = limit_bytes;
    return ldt_install(e, g_sel_base[e], limit_bytes);
}

int ldt_free(uint16_t sel)
{
    int e = sel >> 3;
    if (e <= 0 || e >= 8192)
        return -1;
    g_ldt_used[e] = 0;
    g_sel_base[e] = g_sel_limit[e] = 0;
    struct user_desc d = {0};
    d.entry_number = e;
    d.seg_not_present = 1;
    d.read_exec_only = 1;
    return modify_ldt_write(&d);
}

/* ------------------------------------------------------------- loader -- */

void map_fixed(uint32_t base, uint32_t size, int prot)
{
    uint32_t lo = base & ~0xfffu;
    uint32_t hi = (base + size + 0xfff) & ~0xfffu;
    void *p = mmap((void *)lo, hi - lo, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)lo) {
        LOGE("mmap 0x%x..0x%x failed: %s\n", lo, hi, strerror(errno));
        exit(1);
    }
}

/* load_blob / build_psp_env / enter_game / restore_host_segments load + jump into the ORIGINAL
 * ROTH.EXE object images — used ONLY by the trap-host boot (main()'s #else branch). Under
 * ROTH_STANDALONE there is no object image and no jmp-ENTRY_VA (boot.c drives the image-free boot),
 * so they are dead code image-free; guard them out to keep the imgfree TU warning-free. The trap
 * host (compiled without ROTH_STANDALONE) is unaffected. */
#ifndef ROTH_STANDALONE
static void load_blob(const char *path, uint32_t base, uint32_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOGE("cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    size_t n = fread((void *)base, 1, size, f);
    fclose(f);
    if (n != size) {
        LOGE("%s: short read (%zu != %u)\n", path, n, size);
        exit(1);
    }
}
#endif

const char *g_cmdline = " @ROTH.RES"; /* DOS command tail (leading space) */

#ifndef ROTH_STANDALONE
static void build_psp_env(uint16_t env_sel)
{
    uint8_t *psp = (uint8_t *)PSP_LIN;
    memset(psp, 0, 0x100);
    psp[0] = 0xcd; psp[1] = 0x20;            /* int 20h, traditional */
    *(uint16_t *)(psp + 0x2c) = env_sel;     /* environment selector */
    size_t cl = strlen(g_cmdline);
    if (cl > 126) cl = 126;
    psp[0x80] = (uint8_t)cl;                  /* command-tail length */
    memcpy(psp + 0x81, g_cmdline, cl);        /* the tail itself */
    psp[0x81 + cl] = 0x0d;                    /* CR terminator */

    /* env block: one var, double-NUL, count word, full program path.
     * The Watcom CRT scans vars (looking for no87=), then copies the path. */
    static const char env[] = "ROTHHOST=1\0\0\x01\0C:\\ROTH.EXE";
    memcpy((void *)ENV_LIN, env, sizeof env); /* sizeof includes final NUL */
}

/* -------------------------------------------------------------- entry -- */

static void enter_game(uint16_t psp_sel)
{
    LOGE("entering game at 0x%x, esp=0x%x, psp sel=0x%x\n",
         ENTRY_VA, STACK_TOP, psp_sel);
    uint32_t sel = psp_sel;
    /* Switch to the game's stack and jump. ES = PSP selector per the DPMI
     * startup convention the Watcom CRT expects. No return: the int 21h
     * AH=4Ch handler siglongjmps back. */
    __asm__ volatile(
        "mov %0, %%es\n\t"
        "mov %1, %%esp\n\t"
        "xor %%ebp, %%ebp\n\t"
        "jmp *%2\n\t"
        :
        : "r"(sel), "i"(STACK_TOP), "r"(ENTRY_VA)
        : "memory");
    __builtin_unreachable();
}

static void restore_host_segments(void)
{
    __asm__ volatile(
        "mov %0, %%es\n\t"
        "mov %1, %%ds\n\t"
        "mov %2, %%fs\n\t"
        "mov %3, %%gs\n\t"
        :
        : "r"((uint32_t)g_host_es), "r"((uint32_t)g_host_ds),
          "r"((uint32_t)g_host_fs), "r"((uint32_t)g_host_gs));
}
#endif /* !ROTH_STANDALONE */

/* SIGTERM: dump the mode-13h framebuffer as PPM and exit — headless
 * "screenshot" until the SDL window exists. */
extern uint8_t g_dac_rgb[768];

static void dump_screen(int sig)
{
    (void)sig;
    __asm__ volatile("mov %0, %%fs" :: "r"((uint32_t)g_host_fs));
    __asm__ volatile("mov %0, %%gs" :: "r"((uint32_t)g_host_gs));
    FILE *f = fopen("/tmp/roth_screen.ppm", "wb");
    if (f) {
        /* Dump whatever shm_tick last published (back buffer for Mode-X/VESA,
         * VGA window for plain 320x200), at its actual resolution, so headless
         * screenshots work in every mode. Falls back to the VGA window. */
        int w = 320, h = 200;
        const uint8_t *fb = (const uint8_t *)VGA_LIN;
        const uint8_t *pal = g_dac_rgb;
        if (g_shm && g_shm->cur_w && g_shm->cur_h) {
            w = (int)g_shm->cur_w; h = (int)g_shm->cur_h;
            fb = g_shm->pixels; pal = g_shm->palette;
        }
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; i++) {
            uint8_t px = fb[i];
            fputc(pal[px * 3 + 0] << 2, f);
            fputc(pal[px * 3 + 1] << 2, f);
            fputc(pal[px * 3 + 2] << 2, f);
        }
        fclose(f);
        LOGE("framebuffer (%dx%d) dumped to /tmp/roth_screen.ppm\n", w, h);
    }
    LOGE("traps: int=%lu port_in=%lu port_out=%lu cli/sti=%lu lowmem=%lu other=%lu "
         "(frames=%u)\n",
         g_trap_counts[0], g_trap_counts[1], g_trap_counts[2], g_trap_counts[3],
         g_trap_counts[4], g_trap_counts[5], g_shm ? g_shm->frame : 0);
    { extern unsigned long g_dac_writes; LOGE("  of which DAC(0x3c9) writes=%lu\n", g_dac_writes); }
    { void porthist_dump(void); porthist_dump(); }
    LOGE("blend-variant (0x2dc27) reached %lu times\n", g_blend_reached);
    _exit(0);
}

static void show_eip(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
    __asm__ volatile("mov %0, %%gs" :: "r"((uint32_t)g_host_gs));
    ucontext_t *uc = ucv;
    uint32_t eip = (uint32_t)uc->uc_mcontext.gregs[REG_EIP];
    char buf[96];
    int n = snprintf(buf, sizeof buf, "[probe] eip=0x%x (canon 0x%x)\n", eip,
                     eip - OBJ_DELTA);
    write(2, buf, (size_t)n);
}

int main(int argc, char **argv)
{
    signal(SIGTERM, dump_screen);
    struct sigaction pa = {0};
    pa.sa_sigaction = show_eip;
    pa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGUSR1, &pa, NULL);
    const char *obj_paths[3] = {
        "recomp/build/obj1.bin", "recomp/build/obj2.bin", "recomp/build/obj3.bin",
    };
    int npos = 0;
    int game_dir_given = 0;   /* M4: was --game-dir passed? (else default to the exe's own dir) */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) {
            g_trace = 1;
        } else if (!strcmp(argv[i], "--game-dir") && i + 1 < argc) {
            g_game_dir = argv[++i];
            game_dir_given = 1;
        } else if (!strcmp(argv[i], "--c-root") && i + 1 < argc) {
            g_c_root = argv[++i];
        } else if (!strcmp(argv[i], "--skip-gdv")) {
            g_skip_gdv = 1;
        } else if (!strcmp(argv[i], "--no-hmi386")) {
            g_no_hmi386 = 1;
        } else if (!strcmp(argv[i], "--devmode")) {
            g_devmode = 1;
        } else if (!strcmp(argv[i], "--vesa")) {
            g_vesa = 1;
        } else if (!strcmp(argv[i], "--no-vesa")) {
            g_vesa = -1; /* explicit off (overrides the default-on below) */
        } else if (!strcmp(argv[i], "--probe-blend")) {
            g_probe_blend = 1;
        } else if (!strcmp(argv[i], "--args") && i + 1 < argc) {
            g_cmdline = argv[++i];
#ifdef ROTH_STANDALONE
        } else if (!strcmp(argv[i], "--sdl") || !strcmp(argv[i], "--windowed")) {
            g_sdl_flag = 1;                    /* accepted alias: the window is the M3 default */
        } else if (!strcmp(argv[i], "--headless")) {
            g_headless = 1;                    /* opt into the no-window shm path (rigs/CI) */
        } else if (!strcmp(argv[i], "--sf2") && i + 1 < argc) {
            g_sf2_path = argv[++i];            /* M4: explicit SoundFont (same role as ROTH_SF2) */
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            g_sdl_scale = atoi(argv[++i]);
#ifndef ROTH_VANILLA
        } else if (!strcmp(argv[i], "--no-mods")) {
            g_no_mods = 1;                     /* task #103: safe mode — no plugins */
        } else if (!strcmp(argv[i], "--strict-mods")) {
            g_strict_mods = 1;                 /* task #103: fail the boot on any bad plugin */
        } else if (!strcmp(argv[i], "--list-mods")) {
            g_list_mods = 1;                   /* task #103: discover+validate+print, then exit */
        } else if (!strcmp(argv[i], "--dump-mod-chains")) {
            g_dump_chains = 1;                 /* task #103: print resolved override chains at boot */
#else
        } else if (!strcmp(argv[i], "--no-mods")    || !strcmp(argv[i], "--strict-mods") ||
                   !strcmp(argv[i], "--list-mods")  || !strcmp(argv[i], "--dump-mod-chains")) {
            /* the two-flavor ruling (MODS_PLATFORM.md §12): the vanilla artifact has no mod surface. */
            fprintf(stderr, "[host] %s: vanilla build: mods not supported "
                            "(build the moddable flavor)\n", argv[i]);
#endif
#endif
        } else if (npos < 3) {
            obj_paths[npos++] = argv[i];
        }
    }
    /* VESA on by default (advertises 640x400/640x480 to the game's mode menu);
     * --no-vesa forces the faithful no-VBE / 320x200-only behavior. */
    g_vesa = (g_vesa < 0) ? 0 : 1;
    if (getenv("ROTH_VIDEO_LOG")) g_video_log = 1;     /* log video-mode changes */
    if (getenv("ROTH_FORCE_BLEND")) g_force_blend = 1; /* force-blend experiment */
    if (getenv("ROTH_FORCE_MASK"))                     /* which Q+0xa bit to force */
        g_force_mask = (unsigned)strtol(getenv("ROTH_FORCE_MASK"), NULL, 0);
#ifdef ROTH_STANDALONE
    if (getenv("ROTH_SDL"))      g_sdl_flag = 1;       /* env alias of --sdl (window is default now) */
    if (getenv("ROTH_HEADLESS")) g_headless = 1;       /* env form of --headless (no window, shm on) */
    if (getenv("ROTH_SHM"))      g_want_shm = 1;       /* publish /dev/shm/roth_fb alongside a window */
    if (!g_sdl_scale && getenv("ROTH_SCALE"))
        g_sdl_scale = atoi(getenv("ROTH_SCALE"));
#endif

    __asm__ volatile("mov %%es, %0" : "=r"(g_host_es));
    __asm__ volatile("mov %%ds, %0" : "=r"(g_host_ds));
    __asm__ volatile("mov %%fs, %0" : "=r"(g_host_fs));
    __asm__ volatile("mov %%gs, %0" : "=r"(g_host_gs));

#ifdef ROTH_STANDALONE
    /* Image-free boot: NO obj1/obj2 CODE map, NO int3 live-swap, NO jmp ENTRY_VA. roth_boot()
     * maps the arena + DOS/VGA/VESA, obj3_build's the data, binds the C2/host hooks, arms the always-
     * interactive ISR host-mode + services, and calls roth_main() directly. Never returns to the trap
     * boot below (which is compiled out under ROTH_STANDALONE). SIGTERM -> dump_screen is
     * already armed at the top of main(). */
    (void)obj_paths; (void)npos;

    /* M4: set g_exe_dir; default the game-dir to the exe's own directory unless --game-dir was given.
     * (Exits with a clear message if no CONFIG.INI is found beside the exe nor at the dev path.) */
    resolve_paths(argv[0], game_dir_given);

    /* task #103: hand the plugin loader its options, then honor --list-mods (discover + validate +
     * print the resolved report, then exit BEFORE the game boots — a safe, side-effect-free inspect).
     * MODDABLE flavor only: the vanilla artifact links no plugin loader (the two-flavor ruling). */
#ifndef ROTH_VANILLA
    plugins_configure(g_no_mods, g_strict_mods, g_list_mods, g_dump_chains);
    if (plugins_list_mode()) {
        plugins_discover_report(g_game_dir);
        return 0;
    }
#endif

    /* M3 mode selection (design §6). Show a window unless it's a pure --headless run; publish the
     * /dev/shm/roth_fb framebuffer when headless (always) or ROTH_SHM=1 (both-mode under a window). */
    int want_window = !g_headless || g_sdl_flag;   /* default, --windowed, --sdl, or --headless --sdl */
    int want_shm    = g_headless  || g_want_shm;   /* --headless always; ROTH_SHM adds it to a window */

    if (!want_window) {
        /* --headless / ROTH_HEADLESS: byte-for-byte today's default — the game runs on the main
         * thread, the shm publisher is on (roth_boot()'s own shm_setup() creates /roth_fb), and the
         * external viewer / roth-inject attach as before. No thread, no SDL, no signal-mask change. */
        roth_boot();
        sfx_trace_exit_dump();   /* SFX-dropout trace: headless clean-quit flush (main thread, quiesced) */
        LOGE("game exited with code %d\n", g_exit_code);
        return g_exit_code;
    }

    /* Windowed (or both): Model A — SDL owns the main thread; the game + its SIGALRM tick move to a
     * child thread. Auto-sanity (design §6): prove SDL video can init BEFORE committing the game
     * thread, so a display-less box fails with a clear message instead of silently running windowless
     * (or hanging in pthread_join with no shm to signal quit through). */
    if (!sdl_video_preflight()) {
        LOGE("SDL video init failed — no display/driver available.\n"
             "   Run with --headless (or ROTH_HEADLESS=1) for the no-window shm/CI path.\n");
        return 1;
    }

    /* Pick the framebuffer backing on the main thread BEFORE the game thread starts (design §3/§4):
     * shm_open for the external-attach cases, a private anon map for the windowed default. */
    if (want_shm)
        shm_setup();   /* /dev/shm/roth_fb — window + roth-inject/viewer attach ("both") */
    else
        mem_setup();   /* private in-process framebuffer — windowed, no /dev/shm file */

    /* Block the game-thread signals on main so ITIMER_REAL / SIGTERM / SIGUSR1 are delivered to the
     * game thread (the only one that unblocks them). The child inherits this mask at creation. SDL
     * then owns a signal-quiet main thread (the SDL video subsystem must run on the thread that
     * called SDL_Init). */
    sigset_t block, oldmask;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, &oldmask);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);  /* generous: the game ran on the ~8 MB
                                                          * main stack; give the child room to spare */
    pthread_t game_tid;
    int perr = pthread_create(&game_tid, &attr, game_main, NULL);
    pthread_attr_destroy(&attr);
    if (perr != 0) {
        LOGE("pthread_create(game) failed (%d) — falling back to headless main-thread boot\n", perr);
        pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
        roth_boot();
        sfx_trace_exit_dump();   /* SFX-dropout trace: fallback headless clean-quit flush */
        LOGE("game exited with code %d\n", g_exit_code);
        return g_exit_code;
    }

    /* Present + input loop on the main thread (SDL3). Returns when the window closes or the game
     * thread exits (host_alive -> 0). On window close it sets g_shm->quit, which the game thread's
     * shm_tick honors with _exit(0). */
    sdl_present_run(g_sdl_scale);

    if (g_shm)
        g_shm->quit = 1;   /* belt-and-suspenders: ask the game thread to exit if it hasn't */
    pthread_join(game_tid, NULL);
    LOGE("game exited with code %d\n", g_exit_code);
    return g_exit_code;
#else
    (void)game_dir_given;   /* M4 exe-dir default is ROTH_STANDALONE-only; trap host ignores it */
    /* Object images; obj1 RWX (the CRT patches its own code at 0x43ab1). */
    map_fixed(OBJ1_BASE, OBJ1_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    /* obj2 page + PSP/env/DOS-mem pool share the 0x60000..0x70000 gap. */
    map_fixed(OBJ2_BASE, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC);
    /* obj3 + stack page. */
    map_fixed(OBJ3_BASE, STACK_TOP - OBJ3_BASE + 0xe90, PROT_READ | PROT_WRITE);
    /* DOS-memory pool (int31 0100), below 1 MB linear. */
    map_fixed(DOSMEM_LIN, DOSMEM_SIZE, PROT_READ | PROT_WRITE);
    /* VGA window at its true linear address (mode 13h writes flat 0xA0000). */
    map_fixed(VGA_LIN, VGA_SIZE, PROT_READ | PROT_WRITE);
    /* VESA linear-framebuffer sink (see VESA_LFB_LIN in roth_host.h). */
    map_fixed(VESA_LFB_LIN, VESA_LFB_SIZE, PROT_READ | PROT_WRITE);

    load_blob(obj_paths[0], OBJ1_BASE, OBJ1_SIZE);
    load_blob(obj_paths[1], OBJ2_BASE, OBJ2_SIZE);
    load_blob(obj_paths[2], OBJ3_BASE, OBJ3_SIZE);

    int env_sel = ldt_alloc(ENV_LIN, 0xff);
    int psp_sel = ldt_alloc(PSP_LIN, 0xff);
    if (env_sel < 0 || psp_sel < 0) {
        LOGE("LDT selector allocation failed\n");
        return 1;
    }
    build_psp_env((uint16_t)env_sel);

    /* debug (--probe-blend): do transparency-texture world spans ever reach a
     * blend writer? int3 the texture-descriptor deref (0x368a3) + every blend
     * writer's `mov es/fs,[0x490be2]`; handler gates on the texture having 0x80
     * texels (transparency texture), face-flag-independent. */
    if (g_probe_blend) {
        *(uint8_t *)(uintptr_t)(0x368a3u + OBJ_DELTA) = 0xcc;
        for (int i = 0; i < g_blend_writer_n; i++)
            *(uint8_t *)(uintptr_t)(g_blend_writer_sites[i] + OBJ_DELTA) = 0xcc;
        LOGE("probe int3 patched: deref + %d blend writers\n", g_blend_writer_n);
    }

    /* GDV cutscene frame-boundary hook (see GDV_EMIT_SITE): plant an int3 so the
     * host publishes each COMPLETE decoded frame from the trap handler, instead
     * of catching the decode buffer mid-write on the async timer (which showed
     * half-decoded frames on camera cuts). The clobbered `cmp [..],0` is
     * emulated in the SIGTRAP handler. Harmless when no cutscene plays. */
    extern uint8_t g_gdv_emit_orig;   /* lift_registry.c: real byte under GDV_EMIT_SITE */
    g_gdv_emit_orig = *(uint8_t *)(uintptr_t)(GDV_EMIT_SITE + OBJ_DELTA); /* save before int3 */
    *(uint8_t *)(uintptr_t)(GDV_EMIT_SITE + OBJ_DELTA) = 0xcc;

    lift_install();   /* live-swap ROTH_LIFT-selected functions to verified C */
    calltrace_init(); /* ROTH_TRACE: per-event call-coverage via SIGUSR1/2 */
    capture_init();   /* ROTH_CAPTURE: entry-state snapshots for trace-replay */
    shm_setup();
    traps_install();
    audio_init();
    irq_timer_start();

    if (sigsetjmp(g_exit_jmp, 1) == 0) {
        enter_game((uint16_t)psp_sel);
    }
    restore_host_segments();
    /* Final audio-trace drain. The quit-path veneer calls
     * (close_voices / disable_callback / stop_timer_service / free_driver_slot / unload_detection —
     * sos_audio_shutdown DID dispatch) land in the ring AFTER the last MAGIC_POLL drain and would
     * otherwise die with the process, so a session never saw teardown coverage. Draining
     * HERE captures them. Safe placement: the game has fully unwound (siglongjmp out of enter_game)
     * and host segments are restored, so no game code runs to fault into MAGIC_POLL -> the drain
     * cannot be re-entered concurrently by the timer ISR; au_trace_drain uses write(2) only
     * (async-signal-safe) and is a no-op unless ROTH_AU_TRACE is set. (Preferred over atexit(): this
     * point is a guaranteed main-thread, game-quiesced spot; atexit would run with the itimer still
     * live and no such guarantee.) */
    au_trace_drain();
    sfx_trace_exit_dump();   /* SFX-dropout trace: trap-host clean-quit flush (guaranteed-quiesced spot) */
    LOGE("game exited with code %d\n", g_exit_code);
    return g_exit_code;
#endif /* ROTH_STANDALONE */
}
