/* ROTH native host: shared declarations.
 *
 * The host maps the three LE objects at their original VAs, enters the game,
 * and services DOS/DPMI/BIOS/port traps from a SIGSEGV handler.
 */
#ifndef ROTH_HOST_H
#define ROTH_HOST_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#include "win32_compat.h"   /* register-frame + signal-jump shims for the Windows build */
#else
#include <ucontext.h>
#endif

/* LE layout, rebased like DOS/4GW does at load time: the file-preferred bases
 * (0x10000/0x60000/0x70000) overlap the VGA window and DOS low memory, so the
 * objects live at +OBJ_DELTA (built by roth_reasm.py, default rebase). All
 * analysis docs use the canon (file) addresses: canon = runtime - OBJ_DELTA. */
#define OBJ_DELTA 0x400000u
#define OBJ1_BASE (0x10000u + OBJ_DELTA)
#define OBJ1_SIZE 0x48ccbu
#define OBJ2_BASE (0x60000u + OBJ_DELTA)
#define OBJ2_SIZE 0x13u
#define OBJ3_BASE (0x70000u + OBJ_DELTA)
#define OBJ3_SIZE 0x38170u
#define STACK_TOP (0xa8170u + OBJ_DELTA) /* obj3 end, per LE header */
#define ENTRY_VA  (0x437dcu + OBJ_DELTA)

/* Fake DOS structures live in the unused page gap between obj2 and obj3. */
#define PSP_LIN   (0x68000u + OBJ_DELTA)
#define ENV_LIN   (0x68100u + OBJ_DELTA)
/* Real low memory: VGA window mapped at its true linear address (the game
 * writes flat 0xA0000 in mode 13h), DOS-memory pool below 1 MB so returned
 * real-mode segments (linear >> 4) are meaningful. */
#define VGA_LIN     0xa0000u
#define VGA_SIZE    0x20000u
/* VESA linear-framebuffer sink: a fixed region in the free gap between the VGA
 * window (ends 0xc0000) and obj1 (0x410000). The game maps this as its VBE
 * PhysBasePtr and the present writes here; the host displays the engine's back
 * buffer instead (see traps.c), so it's just a sink that must be mapped. */
#define VESA_LFB_LIN  0x100000u
#define VESA_LFB_SIZE 0x100000u  /* 1 MB: covers 640x480x8 (mapped at 2x = ~600 KB) */
/* DOS conventional-memory pool: the real 640 KB region below the VGA window.
 * Big enough for the game's large (~300 KB) DOS allocs, and every byte has a
 * valid real-mode segment (linear>>4 < 0xa000). Starts at mmap_min_addr. */
#define DOSMEM_LIN  0x10000u
#define DOSMEM_SIZE 0x90000u  /* 0x10000 .. 0xa0000 */

/* Register file view of the trapping context (i386 ucontext). */
typedef struct {
    ucontext_t *uc;
} cpu_t;

uint32_t *reg32(cpu_t *c, int greg);     /* gregs accessor */
#define R_EAX(c) (*reg32(c, REG_EAX))
#define R_EBX(c) (*reg32(c, REG_EBX))
#define R_ECX(c) (*reg32(c, REG_ECX))
#define R_EDX(c) (*reg32(c, REG_EDX))
#define R_ESI(c) (*reg32(c, REG_ESI))
#define R_EDI(c) (*reg32(c, REG_EDI))
#define R_EBP(c) (*reg32(c, REG_EBP))
#define R_ESP(c) (*reg32(c, REG_ESP))
#define R_EIP(c) (*reg32(c, REG_EIP))
#define R_EFL(c) (*reg32(c, REG_EFL))
#define R_ES(c)  (*reg32(c, REG_ES))
#define R_FS(c)  (*reg32(c, REG_FS))
#define R_GS(c)  (*reg32(c, REG_GS))
#define R_DS(c)  (*reg32(c, REG_DS))

#define AH_OF(v) (((v) >> 8) & 0xff)
#define AL_OF(v) ((v) & 0xff)

void set_cf(cpu_t *c, int on);
void set_zf(cpu_t *c, int on);

/* trap dispatch */
void traps_install(void);
void dos_int21(cpu_t *c);
void dpmi_int31(cpu_t *c);
uint32_t dpmi_linear(uint16_t sel, uint32_t off); /* selector:offset -> linear */
uint32_t dpmi_sel_base(uint16_t sel);             /* base of an LDT selector (debug) */
/* Is [lin, lin+len) inside a live DPMI int31-0501 linear allocation? (dpmi.c ledger; task #107 —
 * validates raw far pointers into the SOS song/patch buffers without an address-window guess.) */
int dpmi_lin_alloc_contains(uint32_t lin, uint32_t len);
/* Record the linear base of a host-allocated LDT selector in dpmi.c's software base cache so
 * dpmi_sel_base()/dpmi_linear() resolve it (ldt_alloc sets the real hardware descriptor base but not
 * this cache). Used by the audio open-driver service to bind the far-args selector the lifted
 * voice-field natives translate in software (os_audio.c). */
void dpmi_note_sel_base(uint16_t sel, uint32_t base);
/* DPMI PM interrupt-vector get/set (g_pm_vec[]) for the image-free timer natives' vector install/
 * teardown DPMI arm ([0x755b8]!=0). Dead in-game (that arm is unreached); host-backed for faithfulness. */
uint32_t dpmi_get_pm_vec(uint8_t v);
void     dpmi_set_pm_vec(uint8_t v, uint32_t va);
/* Retune the host IRQ0/int-8 timer to a PIT ch0 divisor from an image-free host-C audio-timer native
 * (the substitute for the SOS timer leaves' `out 0x40` port writes). Mirrors the port-0x40 trap:
 * honor only a nonzero divisor differing from the current; div 0 is a no-op. See traps.c. */
void host_pit_program(uint16_t div);
uint32_t host_lowmem_load(uint32_t ea, int sz);   /* g_lowmem shadow (RM IVT/BDA) — for host_c2 */
void     host_lowmem_store(uint32_t ea, uint32_t val, int sz);
void video_int10(cpu_t *c);
void mouse_int33(cpu_t *c);
void video_publish_composed(void); /* publish the back buffer at a page flip */
void host_flip_video_page(uint32_t eax); /* image-free host present for flip_video_page 0x2e1e8 */
void host_blank_active_video_page(void); /* image-free body for blank_active_video_page 0x2e140 */
/* GDV cutscene frame-boundary hook: int3 here so the host snapshots each
 * COMPLETE decoded frame instead of catching the decode buffer mid-write. */
#define GDV_EMIT_SITE 0x4dcfcu   /* gdv_emit_decoded_frame entry (canon VA) */

/* main.c loader helpers, exposed for the image-free boot (boot.c reuses them) */
void map_fixed(uint32_t base, uint32_t size, int prot);
void shm_setup(void);
void roth_boot(void);   /* src/platform/boot.c — the ROTH_STANDALONE native boot */

/* LDT-backed DPMI selectors */
int ldt_alloc(uint32_t base, uint32_t limit_bytes); /* returns selector or -1 */
int ldt_set_base(uint16_t sel, uint32_t base);
int ldt_set_limit(uint16_t sel, uint32_t limit_bytes);
int ldt_free(uint16_t sel);

/* host services */
extern sigjmp_buf g_exit_jmp;
extern int g_exit_code;
extern const char *g_game_dir;
extern const char *g_c_root; /* DOS C: root; NULL => parent of g_game_dir */
extern int g_trace;
extern int g_skip_gdv;
extern int g_no_hmi386;
extern int g_devmode; /* keep g_dev_mode_flag (canon 0x7f560) set */
extern int g_vesa;    /* enable VESA hi-res (default off; see --vesa) */
extern int g_video_log; /* ROTH_VIDEO_LOG: log video-mode globals on change */
extern int g_probe_blend;          /* debug: count world blend-variant reaches */
extern int g_force_blend;          /* debug: force DAS blend bit on transp spans */
extern unsigned g_force_mask;      /* which Q+0xa bit to force (default 0x400) */
extern const uint32_t g_blend_writer_sites[];
extern const int g_blend_writer_n;
extern unsigned long g_blend_reached;
extern const char *g_cmdline;
extern uint16_t g_host_fs, g_host_gs; /* host TLS selectors (glibc needs gs) */
extern struct roth_shm *g_shm;        /* shared framebuffer/input, or NULL */
extern uint8_t g_dac_rgb[768];        /* captured VGA DAC palette (6-bit) */
void host_dac_port_out(uint16_t port, uint8_t val); /* g_os_port_out target (GDV fmt-1 DAC fade) */
void kbd_enqueue(uint8_t scancode);   /* push a set-1 scancode for IRQ1 */
/* VESA hi-res: mode 0x101 = 640x480x256 banked through the 0xA0000 window */
extern int g_hires;                   /* 0 = mode 13h (320x200), 1 = 640x480 */
void vesa_set_mode(int hires);        /* switch render mode */
void vesa_set_bank(int bank);         /* 4F05: select 64K window bank */
int vesa_get_bank(void);              /* current window bank */
void vesa_set_display_start(uint32_t scanline, uint32_t x); /* 4F07: on-screen LFB page */
/* trap counters by category: 0=int 1=port_in 2=port_out 3=cli/sti 4=lowmem 5=other */
extern unsigned long g_trap_counts[6];
extern uint32_t g_pm_vec_int21[256];  /* vectors installed via int21 AH=25 */
extern volatile int g_soft_if;        /* virtual IF from trapped cli/sti */
extern volatile int g_in_irq;         /* set on inject, cleared on unwind */
extern volatile int g_os_interactive; /* >0 while an interactive lift runs: shm_tick stands in for
                                         * the frozen int-8/int-9 ISRs (frame tick + input ring) */
/* imgfree-boot ISR mode. Set ONLY by the future
 * ROTH_STANDALONE_BOOT=1 boot (roth_boot()) — NOTHING sets it yet, so every path it gates is dead
 * and the trap-host is behaviourally unchanged. When set, alarm_handler forces g_os_interactive=1 so
 * shm_tick drives the lifted C ISR bodies DIRECTLY and then BYPASSES inject_irq — with obj1 unmapped
 * there is no original int-8/int-9 ISR to reflect an IRQ into. */
extern volatile int g_standalone_boot;
extern uint32_t g_irq_eip;            /* game EIP the last timer IRQ preempted */
/* audio-timer quiesce (cluster-7 timer-event-table fence). A future host-C audio-timer native
 * (os_audio_timer_register_event / _remove / configure /
 * stop, in os_audio.c) brackets its edit of the SOS timer-event table
 * (0x979c4/0x979c8/0x97a24/0x97a64/0x97aa4) with au_timer_lock()/au_timer_unlock(). While locked,
 * inject_irq DEFERS — not drops — the master-timer int-8 (whose ISR 0x49eaf reads that table on every
 * tick), so the ISR can never sample a half-written table. The deferred tick is remembered and
 * delivered on the first SIGALRM after unlock (no tick loss, no reentry). This reproduces the PIC IRQ
 * mask (0x54c89/0x54cb5) the original table-editors used and that the host honors on call_orig but
 * that SIGALRM inject_irq otherwise ignores. Timer-IRQ only: keyboard (int-9) and mouse keep flowing.
 * Nesting-safe (g_au_timer_locked is a depth counter, nonzero = locked). No-op and byte-identical to
 * today unless a native calls au_timer_lock() — nothing does yet (timer retirement comes later). */
extern volatile int g_au_timer_locked;
void au_timer_lock(void);             /* enter an audio timer-event-table edit (fences int-8) */
void au_timer_unlock(void);           /* leave it; the next SIGALRM delivers any deferred tick */
/* Far "previous handler" target handed out by AH=35 and used as the far-call
 * return address for injected IRQs; fetching it faults and unwinds. */
#define IRQ_RET_MAGIC 0xffff1000u
void irq_timer_start(void);
/* SIGALRM tick handler (POSIX). The POSIX tick seam installs this via sys_tick_start(); it restores
 * the host TLS selectors, runs the portable tick body (roth_tick_isr), then — when the original image
 * is mapped — services the injected timer/keyboard IRQ. Signal-delivered; POSIX platforms only. */
#ifndef _WIN32
void alarm_handler(int sig, siginfo_t *si, void *ucv);
#endif

#define LOGT(...) do { if (g_trace) fprintf(stderr, "[trap] " __VA_ARGS__); } while (0)
#define LOGE(...) fprintf(stderr, "[host] " __VA_ARGS__)

#endif
