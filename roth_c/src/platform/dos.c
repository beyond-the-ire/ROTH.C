/* int 21h — DOS services. File I/O is forwarded to the host filesystem with
 * DOS->host path translation rooted at --game-dir. Unknown functions dump and
 * exit: the dump is the to-do list. */
#include "roth_host.h"

#include <ctype.h>
#include <fcntl.h>
#ifndef _WIN32
#include <fnmatch.h>   /* the Windows build gets a compact fnmatch via roth_host.h */
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint32_t g_pm_vec_int21[256]; /* handlers installed via AH=25 */
int g_skip_gdv;
int g_no_hmi386;  /* hide .386 HMI drivers (silent sound init) */
int g_devmode;    /* hold the hidden developer flag enabled */
int g_vesa;       /* enable VESA hi-res detection (off: faithful 320x200) */
int g_video_log;  /* ROTH_VIDEO_LOG: log video-mode globals on change */
int g_probe_blend;
int g_force_blend;            /* debug: force a DAS image-type bit on transp spans */
unsigned g_force_mask = 0x400; /* which Q+0xa bit to force (0x400 = TRANSPARENT) */
unsigned long g_blend_reached;
static uint32_t g_dta;

/* Case-insensitive per-component resolution of a DOS path under g_game_dir.
 * Strips drive prefixes; if the leading component doesn't exist (the game's
 * configured root, e.g. "C:\ROTH\..." where game-dir already IS that root),
 * it is skipped once. Returns 0 and fills `out`, or -1. */
#include <dirent.h>

static int resolve_component(char *dir, const char *comp)
{
    char probe[1024];
    snprintf(probe, sizeof probe, "%s/%s", dir, comp);
    if (access(probe, F_OK) == 0) {
        strcpy(dir, probe);
        return 0;
    }
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *e;
    int ok = -1;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, comp)) {
            size_t len = strlen(dir);
            snprintf(dir + len, 1024 - len, "/%s", e->d_name);
            ok = 0;
            break;
        }
    }
    closedir(d);
    return ok;
}

/* DOS drive layout mirrors the shipped DOSBox config (`mount C ".."; cd
 * \roth`): C: root is the PARENT of the game dir, the current directory is the
 * game dir itself. So absolute DOS paths (drive-qualified or leading-slash)
 * resolve under the C: root, and relative paths under the game (current) dir.
 * Override the C: root with --c-root for unusual installs. */
const char *g_c_root;

static const char *c_root(void)
{
    static char root[1024];
    if (g_c_root)
        return g_c_root;
    if (!root[0]) {
        snprintf(root, sizeof root, "%s", g_game_dir);
        size_t n = strlen(root);
        while (n > 1 && root[n - 1] == '/')
            root[--n] = 0;
        char *slash = strrchr(root, '/');
        if (slash && slash != root)
            *slash = 0;
        else
            strcpy(root, ".");
    }
    return root;
}

static int resolve_dos_path(const char *dospath, char *out, size_t outsz,
                            int want_parent_only)
{
    const char *s = dospath;
    int absolute = 0;
    if (s[0] && s[1] == ':') { /* drive letter => absolute */
        s += 2;
        absolute = 1;
    }
    if (*s == '\\' || *s == '/')
        absolute = 1;
    while (*s == '\\' || *s == '/')
        s++;

    char dir[1024];
    snprintf(dir, sizeof dir, "%s", absolute ? c_root() : g_game_dir);
    while (*s) {
        char comp[256];
        size_t n = 0;
        while (*s && *s != '\\' && *s != '/' && n < sizeof comp - 1)
            comp[n++] = *s++;
        comp[n] = 0;
        while (*s == '\\' || *s == '/')
            s++;
        if (*s == 0 && want_parent_only) {
            /* Final component of a create. DOS is case-insensitive, so if a
             * case-variant of this name already exists, REUSE it (overwrite) —
             * otherwise we'd create parallel files (study1.TMP vs STUDY1.TMP)
             * and map/save state would not round-trip. Only when no match
             * exists do we create with the requested name. */
            char saved[1024];
            snprintf(saved, sizeof saved, "%s", dir);
            if (resolve_component(dir, comp) != 0) {
                size_t len = strlen(saved);
                snprintf(saved + len, sizeof saved - len, "/%s", comp);
                snprintf(dir, sizeof dir, "%s", saved);
            }
            break;
        }
        if (resolve_component(dir, comp) != 0)
            return -1;
    }
    snprintf(out, outsz, "%s", dir);
    return 0;
}

static int translate_open(const char *dospath, int flags, mode_t mode)
{
    /* Digital audio (DIGI/HMIDRV.386) is handled by the virtual driver in
     * audio.c; MIDI music is deferred there too (music-init is stubbed). */
    size_t dlen = strlen(dospath);
    /* --no-hmi386: hide ALL HMI .386 drivers (silent sound init; legacy stub). */
    if (g_no_hmi386 && dlen > 4 && !strcasecmp(dospath + dlen - 4, ".386")) {
        LOGT("hiding HMI driver '%s' (--no-hmi386)\n", dospath);
        return -1;
    }
    /* Experiment: hide intro movies to see if the game skips to the menu. */
    if (g_skip_gdv && dlen > 4 && !strcasecmp(dospath + dlen - 4, ".GDV")) {
        LOGT("hiding intro movie '%s' (--skip-gdv)\n", dospath);
        return -1;
    }
    char full[1024];
    if (resolve_dos_path(dospath, full, sizeof full,
                         (flags & O_CREAT) != 0) != 0)
        return -1;
    return open(full, flags, mode);
}

/* ---- DOS findfirst/findnext (AH=4E/4F) -----------------------------------
 * The game enumerates files (e.g. wiping tmp\*.TMP on new game / load, listing
 * saves) via findfirst+findnext. A single active search is kept (DOS usage is
 * a tight loop). Results are written into the caller's DTA in the standard
 * findfirst format so its delete/list loop works. */
#include <dirent.h>
static DIR *g_find_dir;
static char g_find_pat[64];
static char g_find_dirpath[1024];

static void fill_find_dta(const char *name)
{
    if (!g_dta)
        return;
    uint8_t *dta = (uint8_t *)(uintptr_t)g_dta; /* DS flat */
    memset(dta, 0, 0x2b);
    dta[0x15] = 0x20; /* attribute: archive */
    char full[1200];
    snprintf(full, sizeof full, "%s/%s", g_find_dirpath, name);
    struct stat st;
    if (stat(full, &st) == 0)
        *(uint32_t *)(dta + 0x1a) = (uint32_t)st.st_size;
    int i = 0; /* 8.3 uppercase ASCIZ name at +0x1e */
    for (const char *p = name; *p && i < 12; p++)
        dta[0x1e + i++] = (uint8_t)toupper((unsigned char)*p);
    dta[0x1e + i] = 0;
}

static int find_next_match(void)
{
    if (!g_find_dir)
        return -1;
    struct dirent *e;
    while ((e = readdir(g_find_dir))) {
        if (e->d_name[0] == '.')
            continue;
        if (fnmatch(g_find_pat, e->d_name, FNM_CASEFOLD) == 0) {
            fill_find_dta(e->d_name);
            return 0;
        }
    }
    closedir(g_find_dir);
    g_find_dir = NULL;
    return -1;
}

static int dos_findfirst(const char *dospath)
{
    /* split into directory + wildcard pattern (last path component) */
    char path[1024];
    snprintf(path, sizeof path, "%s", dospath);
    char *slash = strrchr(path, '\\');
    char *fwd = strrchr(path, '/');
    if (fwd > slash)
        slash = fwd;
    const char *pat;
    if (slash) {
        *slash = 0;
        pat = slash + 1;
        if (resolve_dos_path(path, g_find_dirpath, sizeof g_find_dirpath, 0) != 0)
            return -1;
    } else {
        pat = path;
        snprintf(g_find_dirpath, sizeof g_find_dirpath, "%s", g_game_dir);
    }
    /* DOS "*.*" means everything */
    snprintf(g_find_pat, sizeof g_find_pat, "%s",
             strcmp(pat, "*.*") == 0 ? "*" : pat);
    if (g_find_dir)
        closedir(g_find_dir);
    g_find_dir = opendir(g_find_dirpath);
    return find_next_match();
}

void dos_int21(cpu_t *c)
{
    uint32_t eax = R_EAX(c);
    uint8_t ah = AH_OF(eax);

    if ((eax & 0xffff) == 0xff00) {
        /* DOS/4G API presence probe from the Watcom CRT: answer "no DOS/4G,
         * use the generic DPMI path". */
        LOGT("int21 ff00 dos4g probe -> AL=ff (absent)\n");
        R_EAX(c) = (eax & 0xffffff00u) | 0xff;
        return;
    }

    switch (ah) {
    case 0x09: { /* print $-terminated string */
        const char *s = (const char *)R_EDX(c);
        size_t n = 0;
        while (s[n] != '$' && n < 4096)
            n++;
        fwrite(s, 1, n, stdout);
        fflush(stdout);
        break;
    }
    case 0x0b: /* check stdin status */
        R_EAX(c) = (eax & ~0xffu); /* AL=0: nothing available */
        break;
    case 0x19: /* current drive: C */
        R_EAX(c) = (eax & ~0xffu) | 2;
        break;
    case 0x1a: /* set DTA */
        g_dta = R_EDX(c);
        break;
    case 0x25: /* set interrupt vector */
        LOGT("int21 25: set vector 0x%02x -> 0x%x\n", AL_OF(eax), R_EDX(c));
        g_pm_vec_int21[AL_OF(eax)] = R_EDX(c);
        break;
    case 0x2a: { /* get date */
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        R_ECX(c) = (R_ECX(c) & ~0xffffu) | (uint32_t)(tm.tm_year + 1900);
        R_EDX(c) = (uint32_t)(((tm.tm_mon + 1) << 8) | tm.tm_mday);
        R_EAX(c) = (eax & ~0xffu) | (uint32_t)tm.tm_wday;
        break;
    }
    case 0x2c: { /* get time */
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        R_ECX(c) = (uint32_t)((tm.tm_hour << 8) | tm.tm_min);
        R_EDX(c) = (uint32_t)(tm.tm_sec << 8);
        break;
    }
    case 0x30: /* DOS version: plain 5.0 (no extender signature) */
        R_EAX(c) = 0x0005;
        R_EBX(c) = 0;
        R_ECX(c) = 0;
        break;
    case 0x35: /* get interrupt vector -> ES:EBX. For never-installed vectors
                * hand out the magic far pointer: Watcom handlers _chain_intr
                * to it, which unwinds the injected IRQ. */
        if (g_pm_vec_int21[AL_OF(eax)]) {
            R_EBX(c) = g_pm_vec_int21[AL_OF(eax)];
        } else {
            R_EBX(c) = IRQ_RET_MAGIC;
        }
        R_ES(c) = (uint32_t)c->uc->uc_mcontext.gregs[REG_CS];
        break;
    case 0x3c: { /* create file */
        int fd = translate_open((const char *)R_EDX(c),
                                O_CREAT | O_TRUNC | O_RDWR, 0644);
        LOGT("int21 3c create '%s' -> %d\n", (const char *)R_EDX(c), fd);
        if (fd < 0) {
            set_cf(c, 1);
            R_EAX(c) = 3;
        } else {
            set_cf(c, 0);
            R_EAX(c) = (uint32_t)fd;
        }
        break;
    }
    case 0x3d: { /* open file: AL access mode */
        static const int modes[] = {O_RDONLY, O_WRONLY, O_RDWR};
        int fd = translate_open((const char *)R_EDX(c), modes[AL_OF(eax) & 3], 0);
        LOGT("int21 3d open '%s' -> %d\n", (const char *)R_EDX(c), fd);
        if (fd < 0) {
            set_cf(c, 1);
            R_EAX(c) = 2; /* file not found */
        } else {
            set_cf(c, 0);
            R_EAX(c) = (uint32_t)fd;
        }
        break;
    }
    case 0x3e: /* close */
        if ((int)R_EBX(c) > 2)
            close((int)R_EBX(c));
        set_cf(c, 0);
        break;
    case 0x3f: { /* read */
        ssize_t n = read((int)R_EBX(c), (void *)R_EDX(c), R_ECX(c));
        LOGT("int21 3f read fd=%d len=%u -> %zd\n", (int)R_EBX(c), R_ECX(c), n);
        if (n < 0) {
            set_cf(c, 1);
            R_EAX(c) = 5;
        } else {
            set_cf(c, 0);
            R_EAX(c) = (uint32_t)n;
        }
        break;
    }
    case 0x40: { /* write */
        int fd = (int)R_EBX(c);
        ssize_t n = write(fd, (const void *)R_EDX(c), R_ECX(c));
        if (n < 0) {
            set_cf(c, 1);
            R_EAX(c) = 5;
        } else {
            set_cf(c, 0);
            R_EAX(c) = (uint32_t)n;
        }
        break;
    }
    case 0x41: { /* delete file (the game manages its own tmp/ + save files) */
        char full[1024];
        const char *dp = (const char *)R_EDX(c);
        if (resolve_dos_path(dp, full, sizeof full, 0) == 0 && unlink(full) == 0) {
            LOGT("int21 41 delete '%s' ok\n", dp);
            set_cf(c, 0);
        } else {
            LOGT("int21 41 delete '%s' -> not found\n", dp);
            set_cf(c, 1);
            R_EAX(c) = 2;
        }
        break;
    }
    case 0x42: { /* lseek: AL whence, CX:DX offset -> DX:AX */
        static const int whence[] = {SEEK_SET, SEEK_CUR, SEEK_END};
        off_t off = (off_t)(int32_t)(((R_ECX(c) & 0xffff) << 16) |
                                     (R_EDX(c) & 0xffff));
        off_t r = lseek((int)R_EBX(c), off, whence[AL_OF(eax) & 3]);
        if (r < 0) {
            set_cf(c, 1);
            R_EAX(c) = 0x19;
        } else {
            set_cf(c, 0);
            R_EAX(c) = (uint32_t)(r & 0xffff);
            R_EDX(c) = (uint32_t)((r >> 16) & 0xffff);
        }
        break;
    }
    case 0x43: /* file attributes: AL=0 get, AL=1 set (accept) */
        set_cf(c, 0);
        if (AL_OF(eax) == 0)
            R_ECX(c) = 0x20; /* archive */
        break;
    case 0x44: /* ioctl: AL=0 get device info */
        if (AL_OF(eax) == 0) {
            int fd = (int)R_EBX(c);
            set_cf(c, 0);
            R_EDX(c) = (fd <= 2) ? 0x80d3u : 0x0002u;
        } else {
            set_cf(c, 0);
            R_EAX(c) = 0;
        }
        break;
    case 0x47: /* getcwd of drive DL into DS:ESI */
        *(char *)R_ESI(c) = 0; /* root */
        set_cf(c, 0);
        break;
    case 0x48: /* alloc paragraphs — shouldn't be hot under DPMI path */
        LOGE("int21 48 alloc %u paras — faking failure\n", R_EBX(c) & 0xffff);
        set_cf(c, 1);
        R_EAX(c) = 8;
        R_EBX(c) = 0;
        break;
    case 0x4a: /* resize memory block: claim success */
        set_cf(c, 0);
        break;
    case 0x4c: /* exit */
        g_exit_code = AL_OF(eax);
        siglongjmp(g_exit_jmp, 1);
        break;
    case 0x4e: { /* findfirst: DS:EDX path+wildcard, fills DTA */
        int ok = dos_findfirst((const char *)R_EDX(c));
        LOGT("int21 4e findfirst '%s' -> %s\n", (const char *)R_EDX(c),
             ok == 0 ? "found" : "none");
        if (ok == 0) {
            set_cf(c, 0);
        } else {
            set_cf(c, 1);
            R_EAX(c) = 0x12; /* no more files */
        }
        break;
    }
    case 0x4f: /* findnext */
        if (find_next_match() == 0) {
            set_cf(c, 0);
        } else {
            set_cf(c, 1);
            R_EAX(c) = 0x12;
        }
        break;
    case 0x56: { /* rename DS:EDX -> ES:EDI (game shuffles tmp/save files) */
        char from[1024], to[1024];
        const char *df = (const char *)R_EDX(c);
        const char *dt = (const char *)dpmi_linear((uint16_t)R_ES(c), R_EDI(c));
        if (resolve_dos_path(df, from, sizeof from, 0) == 0 &&
            resolve_dos_path(dt, to, sizeof to, 1) == 0 && rename(from, to) == 0) {
            LOGT("int21 56 rename '%s' -> '%s' ok\n", df, dt);
            set_cf(c, 0);
        } else {
            LOGT("int21 56 rename '%s' -> '%s' failed\n", df, dt);
            set_cf(c, 1);
            R_EAX(c) = 2;
        }
        break;
    }
    default: {
        LOGE("UNIMPLEMENTED int21 AH=%02x (eax=%08x edx=%08x) at eip 0x%x\n",
             ah, eax, R_EDX(c), R_EIP(c));
        exit(4);
    }
    }
}
