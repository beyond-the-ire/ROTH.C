/* POSIX implementation of the per-OS system seam (see sys.h).
 *
 * The tick is a POSIX interval timer: setitimer(ITIMER_REAL) delivers SIGALRM at
 * the tick period to the thread that has SIGALRM unblocked, and the SIGALRM
 * handler runs the tick. The handler itself (alarm_handler) lives in the host
 * core; this file installs it and arms, retunes, and stops the interval timer.
 */
#include "sys.h"
#include "roth_host.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

void sys_tick_start(unsigned period_us)
{
    struct sigaction sa = {0};
    sa.sa_sigaction = alarm_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, period_us}, {0, period_us}};
    setitimer(ITIMER_REAL, &it, NULL);
}

void sys_tick_set_period(unsigned period_us)
{
    struct itimerval it = {{0, period_us}, {0, period_us}};
    setitimer(ITIMER_REAL, &it, NULL);
}

void sys_tick_stop(void)
{
    struct itimerval it = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &it, NULL);
}

/* ---- dynamic shared-object loading -------------------------------------------
 * dlopen with immediate binding and local scope: the plugin's symbols never join
 * the global namespace, so plugins cannot see one another's symbols. */
void *sys_dlopen(const char *path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void *sys_dlsym(void *handle, const char *symbol)
{
    return dlsym(handle, symbol);
}

void sys_dlclose(void *handle)
{
    dlclose(handle);
}

const char *sys_dlerror(void)
{
    return dlerror();
}

const char *sys_plugin_soname(void)
{
    return "plugin.so";
}

/* ---- low (32-bit-addressable) allocation -------------------------------------
 * A private anonymous mapping constrained to the low 32 bits of the address
 * space. mmap already rounds the length up to a whole number of pages. */
void *sys_lowmem_alloc(size_t len)
{
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

/* ---- executable-page protection ---------------------------------------------- */
int sys_protect_exec(void *addr, size_t len, enum sys_exec_prot prot)
{
    int flags = (prot == SYS_PROT_RWX) ? (PROT_READ | PROT_WRITE | PROT_EXEC)
                                       : (PROT_READ | PROT_EXEC);
    return mprotect(addr, len, flags);
}

/* ---- executable directory ---------------------------------------------------- */
const char *sys_exe_dir(const char *argv0)
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
    return exedir;
}

/* ---- directory queries ------------------------------------------------------- */
int sys_dir_has(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d)))
        if (!strcasecmp(e->d_name, name)) { found = 1; break; }
    closedir(d);
    return found;
}

int sys_enum_dir(const char *dir, int (*cb)(const char *name, void *ud), void *ud)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)))
        if (!cb(e->d_name, ud))
            break;
    closedir(d);
    return 0;
}
