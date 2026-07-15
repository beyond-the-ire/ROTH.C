/* Windows implementation of the per-OS system seam (see sys.h).
 *
 * Windows counterparts of the primitives the portable host layer needs: dynamic
 * library loading, a low (32-bit-addressable) allocator, executable-page
 * protection for self-modifying-code patches, executable-directory and directory
 * queries, the fixed-address arena mapping, and the software descriptor-table
 * ledger. The periodic game tick is not yet provided here — its bodies are
 * present as clearly-marked not-yet stubs; the real timer mechanism is a separate
 * piece of work.
 */
#include "sys.h"
#include "roth_host.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- periodic tick (not yet implemented on this platform) ---------------------
 * The tick drives the whole game clock; until the real timer mechanism lands the
 * arming calls announce themselves loudly (once) and return, so the rest of the
 * boot can be exercised without a running clock. */
static void tick_not_yet(const char *what)
{
    static int announced;
    if (!announced) {
        announced = 1;
        LOGE("periodic tick: %s is not yet implemented on this platform — "
             "the game clock will not advance\n", what);
    }
}

void sys_tick_start(unsigned period_us)
{
    (void)period_us;
    tick_not_yet("sys_tick_start");
}

void sys_tick_set_period(unsigned period_us)
{
    (void)period_us;
    tick_not_yet("sys_tick_set_period");
}

void sys_tick_stop(void)
{
    /* nothing to tear down while the tick is a stub */
}

/* ---- dynamic library loading --------------------------------------------------
 * LoadLibrary/GetProcAddress/FreeLibrary with a read-on-clear error string that
 * mirrors the POSIX loader's "clear, look up, re-read" idiom: the caller clears
 * the state with an ignored read, performs a lookup, then reads again — a non-NULL
 * result means the lookup failed, distinguishing "not found" from "found, value
 * happens to be null". The narrow (A) entry points are used deliberately: the
 * loader works entirely in byte-oriented paths built from the command line and
 * environment, matching how the rest of the host opens files. */
static char g_dlerr[256];
static int  g_dlerr_set;

static void dlerr_capture(void)
{
    DWORD e = GetLastError();
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, e, 0, g_dlerr, (DWORD)sizeof g_dlerr, NULL);
    if (n == 0)
        snprintf(g_dlerr, sizeof g_dlerr, "system error %lu", (unsigned long)e);
    else
        while (n && (g_dlerr[n - 1] == '\n' || g_dlerr[n - 1] == '\r' ||
                     g_dlerr[n - 1] == ' ' || g_dlerr[n - 1] == '.'))
            g_dlerr[--n] = 0;
    g_dlerr_set = 1;
}

void *sys_dlopen(const char *path)
{
    HMODULE h = LoadLibraryA(path);
    if (!h)
        dlerr_capture();
    return (void *)h;
}

void *sys_dlsym(void *handle, const char *symbol)
{
    FARPROC p = GetProcAddress((HMODULE)handle, symbol);
    if (!p)
        dlerr_capture();
    return (void *)p;
}

void sys_dlclose(void *handle)
{
    if (handle)
        FreeLibrary((HMODULE)handle);
}

const char *sys_dlerror(void)
{
    if (!g_dlerr_set)
        return NULL;
    g_dlerr_set = 0;
    return g_dlerr;
}

const char *sys_plugin_soname(void)
{
    return "plugin.dll";
}

/* ---- low (32-bit-addressable) allocation --------------------------------------
 * A committed private reservation. In a 32-bit process every address inherently
 * fits in 32 bits, so no special low-address request is needed. */
void *sys_lowmem_alloc(size_t len)
{
    return VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

/* ---- executable-page protection -----------------------------------------------
 * Open a writable+executable window for a self-modifying-code patch, then restore
 * read+execute. The instruction cache is flushed when returning to the executable
 * state, after the patch bytes have been written, so a core that prefetched the
 * old bytes re-reads the patched ones. */
int sys_protect_exec(void *addr, size_t len, enum sys_exec_prot prot)
{
    DWORD want = (prot == SYS_PROT_RWX) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    DWORD old;
    if (!VirtualProtect(addr, len, want, &old))
        return -1;
    if (prot == SYS_PROT_RX)
        FlushInstructionCache(GetCurrentProcess(), addr, len);
    return 0;
}

/* ---- executable directory -----------------------------------------------------
 * The directory of the running module, with the filename stripped. */
const char *sys_exe_dir(const char *argv0)
{
    static char exedir[1024];
    DWORD n = GetModuleFileNameA(NULL, exedir, (DWORD)sizeof exedir);
    if (n == 0 || n >= sizeof exedir)
        snprintf(exedir, sizeof exedir, "%s", argv0 ? argv0 : ".");
    char *slash = strrchr(exedir, '\\');
    char *fwd = strrchr(exedir, '/');
    if (fwd && (!slash || fwd > slash))
        slash = fwd;
    if (slash)
        *slash = 0;
    else
        strcpy(exedir, ".");
    return exedir;
}

/* ---- directory queries --------------------------------------------------------
 * Enumerate a directory with FindFirstFile/FindNextFile, mirroring the POSIX
 * opendir/readdir seam. The forward-slash separator the host builds its paths with
 * is accepted by the Windows file APIs. */
int sys_dir_has(const char *dir, const char *name)
{
    char pat[1024];
    snprintf(pat, sizeof pat, "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int found = 0;
    do {
        if (_stricmp(fd.cFileName, name) == 0) { found = 1; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

int sys_enum_dir(const char *dir, int (*cb)(const char *name, void *ud), void *ud)
{
    char pat[1024];
    snprintf(pat, sizeof pat, "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        if (!cb(fd.cFileName, ud))
            break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* ---- fixed-address arena mapping ----------------------------------------------
 * Reserve+commit at exactly the requested base. VirtualAlloc with an explicit
 * address returns that address or fails outright (it never relocates), the direct
 * analog of a fail-if-occupied fixed mapping; a miss is fatal, matching the
 * loader's contract that the arena live at its pinned addresses. */
void map_fixed(uint32_t base, uint32_t size, int prot)
{
    uint32_t lo = base & ~0xfffu;
    uint32_t hi = (base + size + 0xfffu) & ~0xfffu;
    DWORD flags = (prot & PROT_EXEC) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    void *p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_COMMIT, flags);
    if (p != (void *)(uintptr_t)lo) {
        LOGE("VirtualAlloc 0x%x..0x%x failed (got %p): %lu\n",
             lo, hi, p, (unsigned long)GetLastError());
        exit(1);
    }
}

/* ---- software descriptor-table ledger -----------------------------------------
 * The far-pointer services resolve selectors to linear addresses purely in
 * software (through the base cache the call sites populate), so a descriptor
 * allocation here only needs to hand back a unique, correctly-encoded selector and
 * remember its base/limit. No hardware descriptor exists on this platform. */
static int      g_ldt_used[8192];
static uint32_t g_sel_base[8192];
static uint32_t g_sel_limit[8192];

int ldt_alloc(uint32_t base, uint32_t limit_bytes)
{
    for (int i = 1; i < 8192; i++) {  /* skip entry 0: keep null-ish low */
        if (!g_ldt_used[i]) {
            g_ldt_used[i] = 1;
            g_sel_base[i] = base;
            g_sel_limit[i] = limit_bytes;
            return (i << 3) | 0x7;     /* LDT, RPL3 */
        }
    }
    return -1;
}

int ldt_set_base(uint16_t sel, uint32_t base)
{
    int e = sel >> 3;
    if (e <= 0 || e >= 8192)
        return -1;
    g_sel_base[e] = base;
    return 0;
}

int ldt_set_limit(uint16_t sel, uint32_t limit_bytes)
{
    int e = sel >> 3;
    if (e <= 0 || e >= 8192)
        return -1;
    g_sel_limit[e] = limit_bytes;
    return 0;
}

int ldt_free(uint16_t sel)
{
    int e = sel >> 3;
    if (e <= 0 || e >= 8192)
        return -1;
    g_ldt_used[e] = 0;
    g_sel_base[e] = g_sel_limit[e] = 0;
    return 0;
}
