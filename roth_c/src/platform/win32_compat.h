/* win32_compat.h — small portability shims for the Windows (MinGW) build.
 *
 * The host layer was written against a POSIX system that provides an i386
 * signal/context register frame, POSIX signal-jump buffers, shell-style filename
 * matching, and memory-protection constants. Windows provides equivalent
 * facilities under different names, and a few of the POSIX ones it does not
 * provide at all. This header supplies just enough of that surface for the
 * portable host code to compile and run on Windows:
 *
 *   - a plain i386 register-frame type (ucontext_t + the REG_* indices). The host
 *     builds SYNTHETIC register frames to reuse its DOS/DPMI service bodies as
 *     ordinary calls (no real signal is involved); this type is that frame's
 *     storage. The OS-delivered-signal consumers of the same type are compiled
 *     out on this platform.
 *   - the POSIX signal-jump buffer mapped onto the plain jump buffer. Without a
 *     signal mask to save or restore there is no distinction to preserve.
 *   - a compact shell-glob matcher for the DOS directory-search service (the
 *     original matched "*.EXT"-style patterns case-insensitively).
 *   - the memory-protection bit constants used at the fixed-mapping seam.
 *
 * Only the Windows build includes this (the whole body is guarded on _WIN32), so
 * it can never shadow the real system headers on any other platform.
 */
#ifndef ROTH_WIN32_COMPAT_H
#define ROTH_WIN32_COMPAT_H

#ifdef _WIN32

#include <setjmp.h>
#include <stddef.h>
#include <time.h>

/* ---- reentrant local-time -----------------------------------------------------
 * The DOS date/time services want the reentrant form. On this single-threaded query
 * path a copy of the shared result is equivalent. */
static inline struct tm *localtime_r(const time_t *t, struct tm *out)
{
    struct tm *r = localtime(t);
    if (!r)
        return NULL;
    *out = *r;
    return out;
}

/* ---- i386 register frame ------------------------------------------------------
 * The index order matches the conventional i386 general-register layout so the
 * REG_* names read naturally; only self-consistency matters here, since the frames
 * on this platform are always built and read by our own code, never handed to us
 * by the operating system. */
typedef int greg_t;
enum {
    REG_GS = 0, REG_FS, REG_ES, REG_DS,
    REG_EDI, REG_ESI, REG_EBP, REG_ESP,
    REG_EBX, REG_EDX, REG_ECX, REG_EAX,
    REG_TRAPNO, REG_ERR, REG_EIP, REG_CS,
    REG_EFL, REG_UESP, REG_SS
};
#define NGREG 19
typedef greg_t gregset_t[NGREG];
typedef struct { gregset_t gregs; } mcontext_t;
typedef struct { mcontext_t uc_mcontext; } ucontext_t;

/* ---- signal-jump buffer -------------------------------------------------------
 * With no signal mask to carry, the signal-aware variants collapse onto the plain
 * jump buffer. */
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(buf, savemask) setjmp(buf)
#define siglongjmp(buf, val)     longjmp((buf), (val))

/* ---- filename matching --------------------------------------------------------
 * A minimal shell-glob matcher covering '*' and '?' plus literals — the only forms
 * the DOS directory-search service produces (e.g. "SAVE*.SAV", "*.DAT"). With the
 * case-fold flag the compare is case-insensitive, matching the DOS filesystem. */
#define FNM_NOMATCH  1
#define FNM_CASEFOLD 0x10

static inline int fnmatch(const char *pat, const char *str, int flags)
{
    int fold = (flags & FNM_CASEFOLD) != 0;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat)
                return 0;                     /* trailing '*' matches the rest */
            for (; *str; str++)
                if (fnmatch(pat, str, flags) == 0)
                    return 0;
            return fnmatch(pat, str, flags);  /* also try matching the empty tail */
        }
        if (*pat == '?') {
            if (!*str)
                return FNM_NOMATCH;
            pat++;
            str++;
            continue;
        }
        {
            char a = *pat, b = *str;
            if (fold) {
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            }
            if (a != b)
                return FNM_NOMATCH;
        }
        pat++;
        str++;
    }
    return *str ? FNM_NOMATCH : 0;
}

/* ---- memory-protection bits ---------------------------------------------------
 * The fixed-mapping seam takes these as its protection argument. */
#ifndef PROT_READ
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif

#endif /* _WIN32 */

#endif /* ROTH_WIN32_COMPAT_H */
