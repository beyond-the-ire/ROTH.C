/* POSIX implementation of the per-OS system seam (see sys.h).
 *
 * The tick is a POSIX interval timer: setitimer(ITIMER_REAL) delivers SIGALRM at
 * the tick period to the thread that has SIGALRM unblocked, and the SIGALRM
 * handler runs the tick. The handler itself (alarm_handler) lives in the host
 * core; this file installs it and arms, retunes, and stops the interval timer.
 */
#include "sys.h"
#include "roth_host.h"

#include <sys/time.h>

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
