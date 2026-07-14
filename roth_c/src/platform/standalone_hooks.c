/* standalone_hooks.c — the trap-free host_soft_int carve-out.
 *
 * host_soft_int lived in lift_registry.c, which carries the call_orig / live-swap undefined the
 * image-free binary must not link. It is itself trap-free (marshals a regs_t into a synthetic ucontext
 * and calls the host int21/int10/int31/int33 services), so it is carved here where BOTH roth-host (via
 * lift_install) and the moddable engine binary (roth) (via roth_boot) reach the ONE definition. The normal build is
 * behaviourally identical — lift_registry.c binds g_os_soft_int to this same body. The image-free-only
 * hooks (roth_unreachable / roth_sprintf) live in boot.c so this TU stays link-safe in the
 * normal host too. */
#include "roth_host.h"
#include "standalone_hooks.h"

#include <string.h>

/* The mod-layer int33 seam (mods/README.md): set by mods_apply() at image-free boot, so it is
 * NULL — dead — in the trap host, which links this TU but never the mod layer. Called after
 * mouse_int33 with the original AX (the int33 subfunction) and the RESULT regs: the mod reads
 * the raw viewer mouse state from them and may suppress what the game sees (overlay input
 * capture). Runs on the game thread (poll_mouse_* per frame) — the mod's safe context for
 * engine calls, unlike the SIGALRM tick. */
void (*g_mods_int33_filter)(uint16_t ax, regs_t *io);

uint32_t host_soft_int(uint8_t vec, regs_t *io)
{
    ucontext_t uc;
    memset(&uc, 0, sizeof uc);
    cpu_t c = { &uc };
    uint16_t ax_in = (uint16_t)io->eax;  /* int33 subfunction for the mod seam below */
    R_EAX(&c) = io->eax; R_EBX(&c) = io->ebx; R_ECX(&c) = io->ecx; R_EDX(&c) = io->edx;
    R_ESI(&c) = io->esi; R_EDI(&c) = io->edi; R_EBP(&c) = io->ebp;
    if (io->es) R_ES(&c) = io->es;   /* DPMI 0300 reads the RM frame from ES:EDI; 0 = unset (all prior callers) */
    switch (vec) {
    case 0x21: dos_int21(&c);   break;
    case 0x10: video_int10(&c); break;
    case 0x31: dpmi_int31(&c);  break;   /* das_assets: free LDT descriptors (AX=0001) */
    case 0x33: mouse_int33(&c); break;   /* input: mouse reset/buttons/motion (lift_input.c) */
    default:   LOGE("host_soft_int: unhandled int 0x%02x\n", vec); break;
    }
    io->eax = R_EAX(&c); io->ebx = R_EBX(&c); io->ecx = R_ECX(&c); io->edx = R_EDX(&c);
    io->esi = R_ESI(&c); io->edi = R_EDI(&c); io->ebp = R_EBP(&c);
    if (vec == 0x33 && g_mods_int33_filter)
        g_mods_int33_filter(ax_in, io);
    return R_EFL(&c);   /* CF = bit 0 (DOS error indicator) */
}
