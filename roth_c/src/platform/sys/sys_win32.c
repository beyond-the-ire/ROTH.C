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

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* ---- the game thread ----------------------------------------------------------
 * The game runs on a thread created here with CreateThread so we own a full-access
 * HANDLE for it: the periodic tick below must suspend exactly this thread, read and
 * rewrite its register context, and resume it. No other thread is ever a suspend
 * target — the structural mirror of "the tick is delivered to the game thread
 * only". The thread's stack bounds are captured (on the thread itself) so the tick
 * can sanity-check a candidate stack pointer before touching the stack. */
static HANDLE    g_game_thread;         /* full-access handle, published before the timer arms */
static size_t    g_game_stack_reserve;  /* the stack reservation, for the bounds computation */
static uintptr_t g_stack_lo, g_stack_hi;/* game-thread stack window [lo, hi) */

/* Adapter: CreateThread's entry is DWORD WINAPI(void*); the portable game entry is
 * void*(void*). The single game thread is started once, so a pair of file statics
 * carries its function and argument across. */
static void *(*g_game_fn)(void *);
static void  *g_game_arg;

static DWORD WINAPI game_thread_entry(LPVOID unused)
{
    (void)unused;
    g_game_fn(g_game_arg);
    return 0;
}

int sys_spawn_game_thread(void *(*fn)(void *), void *arg, size_t stack_bytes)
{
    g_game_fn  = fn;
    g_game_arg = arg;
    g_game_stack_reserve = stack_bytes;
    /* STACK_SIZE_PARAM_IS_A_RESERVATION: stack_bytes is the RESERVE (matching the
     * large reservation the thread is given elsewhere); the OS commits on demand. */
    DWORD tid;
    g_game_thread = CreateThread(NULL, (SIZE_T)stack_bytes, game_thread_entry, NULL,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!g_game_thread) {
        LOGE("CreateThread(game) failed: %lu\n", (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

void sys_join_game_thread(void)
{
    if (g_game_thread)
        WaitForSingleObject(g_game_thread, INFINITE);
}

void sys_game_thread_enter(void)
{
    /* Capture this thread's stack window from its thread-information block: the
     * base (fs:[4]) is the fixed high end; pair it with the reservation for the low
     * bound. No signal mask or selector work — the tick arrives by context-hijack
     * with the thread-information block already correct. */
    uint32_t stack_base;
    __asm__ volatile("movl %%fs:0x4, %0" : "=r"(stack_base));  /* NT_TIB.StackBase */
    g_stack_hi = (uintptr_t)stack_base;
    g_stack_lo = (stack_base > g_game_stack_reserve)
                     ? (uintptr_t)stack_base - g_game_stack_reserve : 0;
}

/* ---- the tick trampoline ------------------------------------------------------
 * The game thread is redirected into this on each accepted tick. The compiler does
 * not support a naked function on this target, so it is a free-standing assembly
 * symbol. On entry Eip is here and Esp points at the pushed return address (the
 * interrupted Eip). Because the hijack interrupts a mid-stream instruction, the
 * interrupted code must see EFLAGS, every GPR and the full x87/SSE state intact on
 * return — the C compiler only preserves callee-saved registers across a call — so
 * this saves and restores all of them around the shared tick body, then returns to
 * the interrupted instruction. The x87/SSE save area is aligned to 16 bytes for
 * fxsave. */
static volatile LONG g_in_tramp;   /* set by the timer thread on hijack; cleared by the body below */

void roth_win_tramp_body(void);
void roth_win_tramp_body(void)
{
    roth_tick_isr();
    g_in_tramp = 0;                /* re-open hijacking (a fire during the epilogue is transparent) */
}

extern void tick_trampoline(void);
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _tick_trampoline\n"
    "_tick_trampoline:\n\t"
        "pushfl\n\t"                 /* save EFLAGS */
        "pushal\n\t"                 /* save EAX..EDI */
        "movl %esp, %ebp\n\t"        /* remember the post-pushal esp (popal restores ebp) */
        "subl $512, %esp\n\t"
        "andl $-16, %esp\n\t"        /* 16-byte-align the 512-byte fxsave area */
        "fxsave (%esp)\n\t"          /* save x87 + SSE (control/status words, MXCSR, register file) */
        "call _roth_win_tramp_body\n\t"
        "fxrstor (%esp)\n\t"
        "movl %ebp, %esp\n\t"        /* undo the align + reservation */
        "popal\n\t"                  /* restore EAX..EDI (including EBP) */
        "popfl\n\t"                  /* restore EFLAGS */
        "ret\n"                      /* pop the pushed original Eip -> resume the interrupted instruction */
);

/* ---- periodic tick ------------------------------------------------------------
 * A dedicated high-resolution waitable-timer thread wakes at the tick period and,
 * when the game thread is at a safe point, redirects it through the trampoline to
 * run the shared per-tick body ON the game thread. This mirrors the original timer
 * interrupt: a periodic, single-threaded preemption of the game.
 *
 * The safe-point test (below) hijacks ONLY when the interrupted instruction is in
 * our own image, the stack pointer is a plausible game-stack address with headroom,
 * and no previous trampoline is still running; otherwise the tick is deferred to the
 * next fire (the game clock tolerates the occasional skipped tick). The "in our own
 * image" bound is PRECOMPUTED once at arm time — the per-fire path calls no
 * potentially-loader-locking API, because the suspended game thread may itself hold
 * the loader lock. */
static uintptr_t g_image_lo, g_image_hi;   /* [lo, hi) of our own mapped image, precomputed */
static HANDLE    g_timer;                  /* the waitable timer */
static HANDLE    g_timer_stop;             /* manual-reset event: ends the timer thread */
static HANDLE    g_timer_thread;
static LONG      g_period_us = 14222;

static void compute_image_bounds(void)
{
    /* One-time, at arm: the exe's own mapped range [base, base + SizeOfImage). Reads
     * only the already-mapped PE headers; taken ONCE here, never in the per-fire path. */
    HMODULE base = GetModuleHandleA(NULL);
    if (!base) { g_image_lo = 0; g_image_hi = 0; return; }
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS *nt  =
        (const IMAGE_NT_HEADERS *)((const char *)base + dos->e_lfanew);
    g_image_lo = (uintptr_t)base;
    g_image_hi = (uintptr_t)base + nt->OptionalHeader.SizeOfImage;
}

/* The safe-point test. All checks are on values already read into `ctx`, plus the
 * precomputed image/stack bounds and the reentrancy flag — no OS call, nothing that
 * could take the loader lock. */
static int hijackable(const CONTEXT *ctx)
{
    if (g_in_tramp)                                    return 0;  /* a prior tick is still running */
    if (ctx->Eip < g_image_lo || ctx->Eip >= g_image_hi) return 0;  /* our own code only */
    if (ctx->Esp > g_stack_hi)                         return 0;  /* a real game-stack pointer ... */
    if (ctx->Esp < g_stack_lo + 0x10000u)              return 0;  /* ... with headroom below it */
    return 1;
}

static void tick_fire(void)
{
    if (!g_game_thread)
        return;
    if (SuspendThread(g_game_thread) == (DWORD)-1)
        return;

    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
    if (GetThreadContext(g_game_thread, &ctx) && hijackable(&ctx)) {
        g_in_tramp = 1;                                     /* cleared by the trampoline body */
        *(uint32_t *)(uintptr_t)(ctx.Esp - 4) = ctx.Eip;    /* push original Eip as a return address */
        ctx.Esp -= 4;
        ctx.Eip  = (DWORD)(uintptr_t)&tick_trampoline;      /* redirect */
        ctx.ContextFlags = CONTEXT_CONTROL;                 /* only Eip/Esp change */
        SetThreadContext(g_game_thread, &ctx);
    }
    ResumeThread(g_game_thread);
}

static void arm_timer(LONG period_us)
{
    if (!g_timer)
        return;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)period_us * 10;   /* relative first fire, in 100 ns units */
    LONG period_ms = (period_us + 500) / 1000;  /* kernel re-fire cadence (ms granularity) */
    if (period_ms < 1)
        period_ms = 1;
    SetWaitableTimer(g_timer, &due, period_ms, NULL, NULL, FALSE);
}

static DWORD WINAPI timer_thread(LPVOID unused)
{
    (void)unused;
    HANDLE waits[2] = { g_timer_stop, g_timer };   /* stop first: it wins a tie */
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0)          /* stop signaled */
            break;
        if (w == WAIT_OBJECT_0 + 1)      /* timer fired */
            tick_fire();
        else                              /* wait error */
            break;
    }
    return 0;
}

void sys_tick_start(unsigned period_us)
{
    compute_image_bounds();
    g_period_us = (LONG)period_us;

    /* The high-resolution waitable timer gives ~1 ms accuracy without raising the
     * global system timer resolution. Fall back to a plain waitable timer on an SKU
     * that rejects the high-resolution flag. */
    g_timer = CreateWaitableTimerExW(NULL, NULL,
                                     CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer)
        g_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    if (!g_timer) {
        LOGE("waitable-timer creation failed: %lu — the game clock will not advance\n",
             (unsigned long)GetLastError());
        return;
    }

    g_timer_stop = CreateEventW(NULL, TRUE, FALSE, NULL);   /* manual-reset */

    DWORD tid;
    g_timer_thread = CreateThread(NULL, 0, timer_thread, NULL, 0, &tid);
    if (!g_timer_thread) {
        LOGE("timer thread creation failed: %lu — the game clock will not advance\n",
             (unsigned long)GetLastError());
        return;
    }
    arm_timer(g_period_us);
}

void sys_tick_set_period(unsigned period_us)
{
    g_period_us = (LONG)period_us;
    arm_timer(g_period_us);   /* audio has its own clock, so image-free this is effectively a no-op */
}

void sys_tick_stop(void)
{
    if (g_timer_stop)
        SetEvent(g_timer_stop);
    if (g_timer_thread) {
        WaitForSingleObject(g_timer_thread, INFINITE);
        CloseHandle(g_timer_thread);
        g_timer_thread = NULL;
    }
    if (g_timer) {
        CancelWaitableTimer(g_timer);
        CloseHandle(g_timer);
        g_timer = NULL;
    }
    if (g_timer_stop) {
        CloseHandle(g_timer_stop);
        g_timer_stop = NULL;
    }
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
