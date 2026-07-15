/* SIGSEGV-based trap dispatch: int NN, cli/sti/hlt, and port I/O all fault in
 * user mode; we decode the faulting instruction, service it, advance EIP, and
 * resume. Anything unrecognized dumps state and exits — that dump is the
 * development worklist. */
#include "roth_host.h"
#include "shared_fb.h"
#include "audio.h"
#include "lift_registry.h"
#include "calltrace.h"
#include "capture.h"
#include "g_names.h"   /* VA_<global> canon-VA constants for the GV8/GV16/GV32 sites (generated) */
#include "sys/sys.h"   /* per-OS tick seam: sys_tick_start/_set_period/_stop + roth_tick_isr */
/* Plugin seam prototypes (task #103 two-flavor ruling): MODDABLE imgfree only — the vanilla flavor
 * (-DROTH_VANILLA) links no plugin loader, so these dispatch calls are compiled out entirely. The
 * plugin loader is the ONLY seam consumer now (the legacy mods layer is retired; docs/MODS_PLATFORM.md
 * §12). Never compiled in the trap host (no -DROTH_STANDALONE there). */
#if defined(ROTH_STANDALONE) && !defined(ROTH_VANILLA)
void plugins_dispatch_compose_tick(uint8_t *pixels, uint32_t w, uint32_t h); /* plugin_loader.c */
uint8_t plugins_dispatch_scancode(uint8_t sc);                               /* plugin_loader.c */
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <setjmp.h>

/* Low-memory (linear < 0x10000) emulation.
 *
 * Real DOS/4GW maps the low 1 MB, so game code freely touches the BIOS data
 * area AND tolerates benign null-pointer reads (e.g. `movsx eax,[ecx+2]` with
 * ecx==0). Linux blocks mapping below mmap_min_addr (0x10000), so those
 * accesses fault. We back the low 64 KB with a zero-initialised shadow buffer
 * and emulate the faulting instruction's memory operand generically via a
 * 32-bit ModRM decoder. This resolves the whole class, not one opcode. */
static uint8_t g_lowmem[0x10000];

static const int GREG8[8] = {REG_EAX, REG_ECX, REG_EDX, REG_EBX,
                             REG_ESP, REG_EBP, REG_ESI, REG_EDI};

static uint32_t lowmem_load(uint32_t ea, int sz)
{
    if (ea == 0x46c && sz == 4) { /* BIOS tick counter, 18.2 Hz */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (uint32_t)((tv.tv_sec * 1000000ull + tv.tv_usec) * 182 / 10000000);
    }
    if (ea + (uint32_t)sz <= sizeof g_lowmem) {
        uint32_t v = 0;
        memcpy(&v, g_lowmem + ea, sz);
        return v;
    }
    return 0;
}

static void lowmem_store(uint32_t ea, uint32_t val, int sz)
{
    if (ea + (uint32_t)sz <= sizeof g_lowmem)
        memcpy(g_lowmem + ea, &val, sz);
}

/* the C2 call-API's window into the same shadow (os_api.c os_lowmem_*): lifted code that
 * pokes the RM IVT / BDA must land in the identical bytes the trapped original lands in */
uint32_t host_lowmem_load(uint32_t ea, int sz)            { return lowmem_load(ea, sz); }
void     host_lowmem_store(uint32_t ea, uint32_t val, int sz) { lowmem_store(ea, val, sz); }

/* Decode a 32-bit ModRM (+SIB+disp) at m. Returns effective address; sets
 * *reg (reg field), *len (bytes consumed by modrm/sib/disp), *direct (mod==3). */
static uint32_t decode_ea(cpu_t *c, uint8_t *m, int *reg, int *len, int *direct)
{
    uint8_t modrm = m[0];
    int mod = modrm >> 6, rm = modrm & 7;
    *reg = (modrm >> 3) & 7;
    *direct = (mod == 3);
    int l = 1;
    uint32_t ea = 0;
    if (mod == 3)
        { *len = l; return 0; }
    if (rm == 4) { /* SIB */
        uint8_t sib = m[l++];
        int idx = (sib >> 3) & 7, bas = sib & 7, scale = 1 << (sib >> 6);
        if (idx != 4)
            ea += (*reg32(c, GREG8[idx])) * (uint32_t)scale;
        if (bas == 5 && mod == 0) { ea += *(int32_t *)(m + l); l += 4; }
        else ea += *reg32(c, GREG8[bas]);
    } else if (rm == 5 && mod == 0) {
        ea = *(uint32_t *)(m + l); l += 4;
    } else {
        ea = *reg32(c, GREG8[rm]);
    }
    if (mod == 1) { ea += (int8_t)m[l]; l += 1; }
    else if (mod == 2) { ea += *(int32_t *)(m + l); l += 4; }
    *len = l;
    return ea;
}

/* Write an 8-bit value into the correct byte of a register (reg 0-3 = low byte
 * of eax/ecx/edx/ebx; 4-7 = ah/ch/dh/bh). */
static void set_r8(cpu_t *c, int reg, uint8_t v)
{
    uint32_t *r = reg32(c, GREG8[reg & 3]);
    if (reg < 4)
        *r = (*r & ~0xffu) | v;
    else
        *r = (*r & ~0xff00u) | ((uint32_t)v << 8);
}

static uint8_t get_r8(cpu_t *c, int reg)
{
    uint32_t r = *reg32(c, GREG8[reg & 3]);
    return reg < 4 ? (uint8_t)r : (uint8_t)(r >> 8);
}

static void set_rN(cpu_t *c, int reg, uint32_t v, int sz)
{
    uint32_t *r = reg32(c, GREG8[reg]);
    if (sz == 2) *r = (*r & ~0xffffu) | (v & 0xffff);
    else *r = v;
}

/* Emulate a faulting low-memory access. p -> opcode (after prefixes),
 * pfx_len = prefix byte count, opsize = 2 or 4. Returns 1 if handled. */
static int emulate_lowmem(cpu_t *c, uint8_t *p, int opsize, uint32_t pfx_len)
{
    uint32_t eip = R_EIP(c);
    int reg, len, direct;
    uint8_t op = p[0];

    switch (op) {
    case 0x8b: { /* mov r(16/32), rm */
        uint32_t ea = decode_ea(c, p + 1, &reg, &len, &direct);
        if (direct) return 0;
        set_rN(c, reg, lowmem_load(ea, opsize), opsize);
        R_EIP(c) = eip + pfx_len + 1 + len; return 1;
    }
    case 0x8a: { /* mov r8, rm8 */
        uint32_t ea = decode_ea(c, p + 1, &reg, &len, &direct);
        if (direct) return 0;
        set_r8(c, reg, (uint8_t)lowmem_load(ea, 1));
        R_EIP(c) = eip + pfx_len + 1 + len; return 1;
    }
    case 0x89: { /* mov rm, r(16/32) */
        uint32_t ea = decode_ea(c, p + 1, &reg, &len, &direct);
        if (direct) return 0;
        lowmem_store(ea, *reg32(c, GREG8[reg]), opsize);
        R_EIP(c) = eip + pfx_len + 1 + len; return 1;
    }
    case 0x88: { /* mov rm8, r8 */
        uint32_t ea = decode_ea(c, p + 1, &reg, &len, &direct);
        if (direct) return 0;
        lowmem_store(ea, get_r8(c, reg), 1);
        R_EIP(c) = eip + pfx_len + 1 + len; return 1;
    }
    case 0xc6: { /* mov rm8, imm8 */
        uint32_t ea = decode_ea(c, p + 1, &reg, &len, &direct);
        if (direct) return 0;
        lowmem_store(ea, p[1 + len], 1);
        R_EIP(c) = eip + pfx_len + 1 + len + 1; return 1;
    }
    case 0xc7: { /* mov rm(16/32), imm */
        uint32_t ea = decode_ea(c, p + 1, &reg, &len, &direct);
        if (direct) return 0;
        uint32_t imm = 0; memcpy(&imm, p + 1 + len, opsize);
        lowmem_store(ea, imm, opsize);
        R_EIP(c) = eip + pfx_len + 1 + len + opsize; return 1;
    }
    case 0xa0: /* mov al, moffs8 */
        R_EAX(c) = (R_EAX(c) & ~0xffu) | (lowmem_load(*(uint32_t *)(p + 1), 1) & 0xff);
        R_EIP(c) = eip + pfx_len + 5; return 1;
    case 0xa1: /* mov eAX, moffs */
        set_rN(c, 0, lowmem_load(*(uint32_t *)(p + 1), opsize), opsize);
        R_EIP(c) = eip + pfx_len + 5; return 1;
    case 0xa2: /* mov moffs8, al */
        lowmem_store(*(uint32_t *)(p + 1), R_EAX(c) & 0xff, 1);
        R_EIP(c) = eip + pfx_len + 5; return 1;
    case 0xa3: /* mov moffs, eAX */
        lowmem_store(*(uint32_t *)(p + 1), R_EAX(c), opsize);
        R_EIP(c) = eip + pfx_len + 5; return 1;
    case 0x0f: { /* two-byte: movzx/movsx */
        uint8_t op2 = p[1];
        if (op2 == 0xb6 || op2 == 0xbe || op2 == 0xb7 || op2 == 0xbf) {
            uint32_t ea = decode_ea(c, p + 2, &reg, &len, &direct);
            if (direct) return 0;
            int srcsz = (op2 == 0xb6 || op2 == 0xbe) ? 1 : 2;
            uint32_t v = lowmem_load(ea, srcsz);
            if (op2 == 0xbe) v = (uint32_t)(int32_t)(int8_t)(uint8_t)v;
            else if (op2 == 0xbf) v = (uint32_t)(int32_t)(int16_t)(uint16_t)v;
            set_rN(c, reg, v, opsize);
            R_EIP(c) = eip + pfx_len + 2 + len; return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

uint32_t *reg32(cpu_t *c, int greg)
{
    return (uint32_t *)&c->uc->uc_mcontext.gregs[greg];
}

void set_cf(cpu_t *c, int on)
{
    if (on)
        R_EFL(c) |= 1u;
    else
        R_EFL(c) &= ~1u;
}

void set_zf(cpu_t *c, int on)
{
    if (on)
        R_EFL(c) |= 1u << 6;
    else
        R_EFL(c) &= ~(1u << 6);
}

static void dump_and_die(cpu_t *c, const char *why)
{
    uint32_t eip = R_EIP(c);
    /* Capture the screen at the moment of death (host segments are active). */
    if (g_shm) {
        memcpy(g_shm->palette, g_dac_rgb, sizeof g_shm->palette);
        /* Only VGA_SIZE (0x20000) is mapped at VGA_LIN; pixels[] is larger
         * (hi-res capacity). Copying sizeof(pixels) overruns the window and
         * faults recursively inside the crash dumper. Clamp to the window. */
        memcpy(g_shm->pixels, (const void *)(uintptr_t)VGA_LIN, VGA_SIZE);
        g_shm->frame++;
        FILE *f = fopen("/tmp/roth_crash.ppm", "wb");
        if (f) {
            fprintf(f, "P6\n320 200\n255\n");
            const uint8_t *fb = (const uint8_t *)(uintptr_t)VGA_LIN;
            for (int i = 0; i < 320 * 200; i++) {
                uint8_t px = fb[i];
                fputc(g_dac_rgb[px * 3] << 2, f);
                fputc(g_dac_rgb[px * 3 + 1] << 2, f);
                fputc(g_dac_rgb[px * 3 + 2] << 2, f);
            }
            fclose(f);
            LOGE("crash framebuffer -> /tmp/roth_crash.ppm\n");
        }
    }
    LOGE("FATAL: %s\n", why);
    /* GDV-decoder globals (runtime VAs = canon + OBJ_DELTA). */
    LOGE("gdv: [74578]=%08x [97b54]=%08x [97b58]=%08x [9740c]=%08x [97824]=%08x\n",
         *(uint32_t *)(uintptr_t)(0x74578 + OBJ_DELTA),
         *(uint32_t *)(uintptr_t)(0x97b54 + OBJ_DELTA),
         *(uint32_t *)(uintptr_t)(0x97b58 + OBJ_DELTA),
         *(uint32_t *)(uintptr_t)(0x9740c + OBJ_DELTA),
         *(uint32_t *)(uintptr_t)(0x97824 + OBJ_DELTA));
    LOGE("obj: [81054]=%08x [81e1c]=%08x [81e20]=%08x [81038]=%08x [8104c]=%08x\n",
         *(uint32_t *)(uintptr_t)(0x81054 + OBJ_DELTA),
         *(uint32_t *)(uintptr_t)(0x81e1c + OBJ_DELTA),
         *(uint32_t *)(uintptr_t)(0x81e20 + OBJ_DELTA),
         *(uint32_t *)(uintptr_t)(0x81038 + OBJ_DELTA),
         *(uint32_t *)(uintptr_t)(0x8104c + OBJ_DELTA));
    LOGE("eip=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
         eip, R_EAX(c), R_EBX(c), R_ECX(c), R_EDX(c));
    LOGE("esi=%08x edi=%08x ebp=%08x esp=%08x efl=%08x\n",
         R_ESI(c), R_EDI(c), R_EBP(c), R_ESP(c), R_EFL(c));
    LOGE("cs=%04x ds=%04x es=%04x fs=%04x gs=%04x ss=%04x\n",
         (uint16_t)c->uc->uc_mcontext.gregs[REG_CS],
         (uint16_t)c->uc->uc_mcontext.gregs[REG_DS],
         (uint16_t)c->uc->uc_mcontext.gregs[REG_ES],
         (uint16_t)c->uc->uc_mcontext.gregs[REG_FS],
         (uint16_t)c->uc->uc_mcontext.gregs[REG_GS],
         (uint16_t)c->uc->uc_mcontext.gregs[REG_SS]);
    if (eip >= OBJ1_BASE && eip < OBJ1_BASE + OBJ1_SIZE) {
        uint8_t *p = (uint8_t *)eip;
        LOGE("bytes: %02x %02x %02x %02x %02x %02x %02x %02x "
             "%02x %02x %02x %02x\n",
             p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
             p[10], p[11]);
        LOGE("(canon VA 0x%x — look up in recomp/asm/roth.s / Ghidra)\n",
             eip - OBJ_DELTA);
    }
    /* Raw stack dump near ESP for true caller/args (canon-adjusted). */
    {
        uint32_t sp0 = R_ESP(c);
        for (int i = 0; i < 10 && sp0 + i * 4 < STACK_TOP; i++) {
            uint32_t v = *(uint32_t *)(sp0 + i * 4);
            int isret = (v >= OBJ1_BASE && v < OBJ1_BASE + OBJ1_SIZE &&
                         ((uint8_t *)(uintptr_t)(v - 5))[0] == 0xe8);
            LOGE("  [esp+%02x]=%08x%s%s\n", i * 4, v,
                 (v >= OBJ1_BASE && v < OBJ3_BASE) ? " canon=" : "",
                 "");
            if (isret)
                LOGE("       ^ return into canon 0x%x\n", v - OBJ_DELTA);
        }
    }
    /* Watcom frames save regs before ebp, so scan the stack for dwords that
     * point just-after a `call` in obj1 — candidate return addresses. */
    uint32_t sp = R_ESP(c);
    int shown = 0;
    for (uint32_t a = sp; a < STACK_TOP && shown < 16; a += 4) {
        uint32_t v = *(uint32_t *)a;
        if (v >= OBJ1_BASE && v < OBJ1_BASE + OBJ1_SIZE) {
            uint8_t *pc = (uint8_t *)(uintptr_t)(v - 5);
            if (pc[0] == 0xe8) { /* preceded by a near call: real return addr */
                LOGE("  bt ret=0x%x (canon 0x%x)\n", v, v - OBJ_DELTA);
                shown++;
            }
        }
    }
    exit(3);
}

static void dispatch_int(cpu_t *c, uint8_t vec)
{
    switch (vec) {
    case 0x21: dos_int21(c); break;
    case 0x31: dpmi_int31(c); break;
    case 0x10: video_int10(c); break;
    case 0x33: mouse_int33(c); break;
    case 0x2f:
        /* DPMI/Windows detection: report "nothing special present". */
        LOGT("int 2f ax=%04x -> not present\n", R_EAX(c) & 0xffff);
        break;
    default: {
        char msg[64];
        snprintf(msg, sizeof msg, "unhandled int 0x%02x ax=%04x", vec,
                 R_EAX(c) & 0xffff);
        dump_and_die(c, msg);
    }
    }
}

/* Port I/O: mostly stubs (return float-high on reads), but the VGA DAC
 * palette writes are captured so screenshots have real colors. Real device
 * models (PIT, PIC, SB) arrive with the timer/audio milestones. */
uint8_t g_dac_rgb[768]; /* 6-bit VGA DAC values */
static int g_dac_pos;
unsigned long g_trap_counts[6];
unsigned long g_dac_writes;
static int g_pit_lohi;       /* PIT 0x40 byte toggle (0=lo next, 1=hi next) */
uint16_t g_pit_div;          /* captured PIT ch0 divisor (HMI audio timer rate) */

/* Retune the host int8/IRQ0 timer to the PIT ch0 divisor the game programs, so the
 * game's tick-driven timing AND the MIDI sequencer's tempo run at the rate the game
 * expects (MIDI bumps it 70->120 Hz; the host used to stay pinned at 70 Hz, starving
 * everything tick-driven to ~58% speed). rate = 1193182/div Hz; interval clamped to
 * a sane 5..1000 Hz. EXPERIMENTAL — honors the request faithfully; see traps.c notes
 * about the 70 Hz-assuming present/audio-pacing paths. */
static void pit_set_rate(uint16_t div)
{
    if (!div) return;
    long us = (long)((double)div * 1000000.0 / 1193182.0);
    if (us < 1000)   us = 1000;     /* cap ~1000 Hz */
    if (us > 200000) us = 200000;   /* floor ~5 Hz */
    sys_tick_set_period((unsigned)us);
}

/* Program the host IRQ0/int-8 timer to a PIT ch0 divisor from an image-free host-C audio-timer native
 * (0x4980d/0x498e9/0x49923/0x49ca4) — the substitute for the SOS leaf's `out 0x40` port writes
 * (sos_program_pit_divisor 0x49e7a -> 0x54b81, and the 0x54bc7/0x54ce1 install/teardown tails). It
 * mirrors the port-0x40 trap (see the `port == 0x40` case below) EXACTLY: honor only a NONZERO divisor
 * that DIFFERS from the current one (the trap ignores div 0 and identical re-writes), update g_pit_div,
 * retune setitimer, LOGE. So host_pit_program(0) is a deliberate no-op — matching the teardown's
 * `out 0x40,0` being ignored by the trap, i.e. the host keeps its current SIGALRM cadence. */
void host_pit_program(uint16_t div)
{
    if (div && div != g_pit_div) {
        g_pit_div = div;
        pit_set_rate(div);
        LOGE("PIT ch0 divisor=%u -> %.1f Hz (host-C timer native) [host timer retuned]\n",
             div, 1193182.0 / div);
    }
}

/* histogram of port-out trap EIPs (find hot palette/VGA loops) */
#define PORTHIST_N 48
static uint32_t g_porthist_eip[PORTHIST_N];
static unsigned long g_porthist_cnt[PORTHIST_N];
static void porthist_add(uint32_t eip)
{
    for (int i = 0; i < PORTHIST_N; i++) {
        if (g_porthist_eip[i] == eip) { g_porthist_cnt[i]++; return; }
        if (g_porthist_eip[i] == 0) { g_porthist_eip[i] = eip; g_porthist_cnt[i] = 1; return; }
    }
}
void porthist_dump(void); /* called from SIGTERM dump */
void porthist_dump(void)
{
    for (int i = 0; i < PORTHIST_N && g_porthist_eip[i]; i++)
        if (g_porthist_cnt[i] > 200)
            fprintf(stderr, "[host]   out-trap canon 0x%x : %lu\n",
                    g_porthist_eip[i] - OBJ_DELTA, g_porthist_cnt[i]);
}
static volatile int g_irq_active; /* nonzero between IRQ inject and EOI */

/* 8042 keyboard model: scancodes queued here, delivered via port 0x60; an
 * IRQ1 (int 9) is injected per scancode so the game's handler reads them. */
static volatile uint8_t g_kbd_q[64];
static volatile int g_kbd_head, g_kbd_tail;

void kbd_enqueue(uint8_t sc)
{
    int next = (g_kbd_head + 1) & 63;
    if (next != g_kbd_tail) {
        g_kbd_q[g_kbd_head] = sc;
        g_kbd_head = next;
    }
}

static uint8_t kbd_peek(void) /* current byte at port 0x60 */
{
    if (g_kbd_tail == g_kbd_head)
        return 0;
    return g_kbd_q[g_kbd_tail];
}

static int kbd_pending(void) { return g_kbd_tail != g_kbd_head; }

static void kbd_pop(void)
{
    if (g_kbd_tail != g_kbd_head)
        g_kbd_tail = (g_kbd_tail + 1) & 63;
}

/* ---- task #15 (P3 host input bug): released movement key persists ~0.5s under interactive swaps ----
 * A break (release) scancode drained by a NON-interactive shm_tick lands in the 8042 queue (g_kbd_q),
 * which only the game's real int-9 ISR drains — via inject_irq(0x09), which BAILS on g_in_handler for
 * the whole duration of any lift dispatch. The interactive stand-in (shm_tick, g_os_interactive) only
 * read the shm ring, never g_kbd_q, so a break stranded in g_kbd_q kept the held-key bit @0x90c3c SET
 * for the dispatch's length -> the player kept moving after release. Instrumentation + a reclaim fix:
 *   ROTH_KBD_TRACE=1      : log which path applied each scancode + every suppressed IRQ9 (the stranding).
 *   ROTH_KBD_NO_RECLAIM=1 : disable the fix (interactive-path g_kbd_q reclaim) to REPRODUCE the stick. */
static int g_kbd_trace = -1;
static int g_kbd_no_reclaim = -1;
static unsigned long g_irq9_supp_inhandler, g_irq9_supp_inirq, g_irq9_supp_softif, g_irq9_supp_other;
static int kbd_trace_on(void)
{ if (g_kbd_trace < 0) g_kbd_trace = getenv("ROTH_KBD_TRACE") ? 1 : 0; return g_kbd_trace; }
static int kbd_reclaim_on(void)
{ if (g_kbd_no_reclaim < 0) g_kbd_no_reclaim = getenv("ROTH_KBD_NO_RECLAIM") ? 1 : 0; return !g_kbd_no_reclaim; }

static void port_in(cpu_t *c, uint16_t port, int size)
{
    uint32_t v = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
    if (port == 0x3da) {
        /* Input status 1: vretrace (bit3) + display-enable (bit0). A vsync wait
         * polls this from a FIXED EIP, so the value must change across reads or
         * the loop spins forever (the 640x480 path does exactly this). Drive
         * both bits from a free-running per-read counter so any wait shape —
         * "until vretrace", "until display", or a full frame — terminates. */
        static unsigned vga_stat;
        vga_stat++;
        v = ((vga_stat & 7) == 0 ? 0x08 : 0x00) | (vga_stat & 1);
    }
    else if (port == 0x60) { /* keyboard data port */
        v = kbd_peek();
        kbd_pop();
        if (v && kbd_trace_on()) {
            static int n; if (n++ < 200)
                LOGE("[kbd] port60 pop sc=0x%02x %s (real int-9 ISR drained 8042) frame=%u\n",
                     (unsigned)v, (v & 0x80) ? "BREAK" : "make", g_shm ? g_shm->frame : 0);
        }
    } else if (port == 0x64) /* 8042 status: bit0=output buffer full */
        v = kbd_pending() ? 0x01 : 0x00;
    LOGT("in  port 0x%03x size %d -> 0x%x (eip 0x%x)\n", port, size, v, R_EIP(c));
    uint32_t mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
    R_EAX(c) = (R_EAX(c) & ~mask) | (v & mask);
}

static uint8_t g_crtc_index; /* last CRTC index selected via port 0x3d4 */

static void port_out(cpu_t *c, uint16_t port, int size)
{
    uint8_t al = (uint8_t)R_EAX(c);
    if (port == 0x20 && al == 0x20) { /* PIC EOI: IRQ handler is done */
        g_in_irq = 0;
        g_irq_active = 0;
        return;
    }
    if (port == 0x3c8) {
        g_dac_pos = al * 3;
        return;
    }
    if (port == 0x3c9) {
        g_dac_writes++;
        g_dac_rgb[g_dac_pos % 768] = al;
        g_dac_pos = (g_dac_pos + 1) % 768;
        return;
    }
    /* PIT channel 0 divisor capture (the HMI mixer reprograms the timer rate;
     * we need it to play digital audio at the right speed). Mode write to 0x43
     * resets the lo/hi byte toggle; two bytes to 0x40 form the divisor. */
    if (port == 0x43) {
        if (((al >> 6) & 3) == 0) /* channel 0 */
            g_pit_lohi = 0;
        return;
    }
    /* CRTC (0x3d4 index / 0x3d5 data): the Mode-X page flip rewrites the display
     * start address (regs 0x0c/0x0d). flip_video_page writes 0x0d then 0x0c, so
     * publishing the (now-complete) back buffer on the 0x0c data write syncs the
     * frame to the flip — see video_publish_composed. */
    if (port == 0x3d4) { g_crtc_index = al; return; }
    if (port == 0x3d5) {
        if (g_crtc_index == 0x0c)
            video_publish_composed();
        return;
    }
    if (port == 0x40) {
        static uint16_t div;
        if (!g_pit_lohi) { div = al; g_pit_lohi = 1; }
        else {
            div |= (uint16_t)al << 8;
            g_pit_lohi = 0;
            if (div && div != g_pit_div) {
                g_pit_div = div;
                pit_set_rate(div);   /* EXPERIMENT: honor the game's PIT rate (was pinned at 70 Hz) */
                LOGE("PIT ch0 divisor=%u -> %.1f Hz (canon 0x%x) [host timer retuned]\n", div,
                     1193182.0 / div, R_EIP(c) - OBJ_DELTA);
            }
        }
        return;
    }
    LOGT("out port 0x%03x size %d val 0x%x (eip 0x%x)\n", port, size,
         R_EAX(c) & (size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu),
         R_EIP(c));
}

/* Public byte-port-out for the g_os_port_out hook: a lift running native code can't execute the
 * privileged `out` instruction, so it routes its DAC writes (0x3c8 index / 0x3c9 RGB) through here,
 * sharing the same g_dac_pos / g_dac_rgb state the trap path uses. The GDV codec's format-1 palette
 * fade is the only user. Mirrors the 0x3c8/0x3c9 branches of port_out exactly. */
void host_dac_port_out(uint16_t port, uint8_t val)
{
    if (port == 0x3c8) { g_dac_pos = val * 3; return; }
    if (port == 0x3c9) {
        g_dac_writes++;
        g_dac_rgb[g_dac_pos % 768] = val;
        g_dac_pos = (g_dac_pos + 1) % 768;
        return;
    }
}

/* VGA palette-upload fast path.
 *
 * ROTH has several DAC upload loops that write one byte per `out` instruction
 * (index to 0x3c8, then R,G,B to 0x3c9): the main upload (canon 0x4c36e) and
 * the fade/flash loops (0x2fede direct, 0x2ff10 with an 8->6-bit shr). Each is
 * ~1024 signal traps per 256-colour pass — the dominant per-event cost and the
 * source of damage-flash / fade hitching. When we fault on a loop's index
 * write, replay the whole loop in C and jump past it: one trap, not a thousand.
 *
 * All share the shape: at the `out dx,al` with dx==0x3c8, AL/BL = start index,
 * EDI = RGB source, (E)CX = colour count; RGB read from [EDI] (>> `shift`). */
struct pal_loop { uint32_t trig, end; int shift; };
static const struct pal_loop g_pal_loops[] = {
    {0x4c374u, 0x4c389u, 0}, /* main palette upload */
    {0x2fee2u, 0x2fef8u, 0}, /* fade/flash (6-bit source) */
    {0x2ff14u, 0x2ff33u, 2}, /* fade/flash (8-bit source >>2) */
};

static int palette_fastpath(cpu_t *c)
{
    if ((R_EDX(c) & 0xffff) != 0x3c8)
        return 0;
    uint32_t eip = R_EIP(c);
    for (unsigned k = 0; k < sizeof g_pal_loops / sizeof g_pal_loops[0]; k++) {
        if (eip != g_pal_loops[k].trig + OBJ_DELTA)
            continue;
        int shift = g_pal_loops[k].shift;
        uint32_t start = R_EAX(c) & 0xff;
        uint32_t count = R_ECX(c) & 0xffff;
        uint32_t src = R_EDI(c);
        const uint8_t *s = (const uint8_t *)(uintptr_t)src;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t d = ((start + i) & 0xff) * 3;
            g_dac_rgb[d] = s[i * 3] >> shift;
            g_dac_rgb[d + 1] = s[i * 3 + 1] >> shift;
            g_dac_rgb[d + 2] = s[i * 3 + 2] >> shift;
        }
        g_dac_writes += count * 3;
        R_EDI(c) = src + 3 * count;
        R_EBX(c) = R_EBX(c) + count;
        R_ECX(c) = R_ECX(c) & ~0xffffu;
        R_EDX(c) = (R_EDX(c) & ~0xffffu) | 0x3c9;
        R_EIP(c) = g_pal_loops[k].end + OBJ_DELTA;
        return 1;
    }
    return 0;
}

static volatile int g_in_handler;
/* Set (incremented) by lift_dispatch in lift_registry.c while an INTERACTIVE lift is running — a
 * menu/prompt that internally spins on the frame-tick counter (0x90bcc) and/or waits on the game
 * input ring (0x90c1c). During ANY lift dispatch the game's int-8/int-9 ISRs are frozen: inject_irq
 * bails on g_in_handler, and even if it didn't, call_orig runs the bridged original on the host
 * SIGNAL ALTSTACK whose esp fails inject_irq's game-stack guard — so an injected IRQ frame could
 * never reach the spinning code. Such a lift would therefore deadlock (the trap handler must return
 * for the game loop to advance, but the menu blocks on interrupt-driven state). While this flag is
 * set, shm_tick (still pumped by SIGALRM, independent of g_in_handler) stands in for the two frozen
 * ISRs directly: it advances the frame tick and forwards scancodes into the game input ring. */
volatile int g_os_interactive;
static volatile uint32_t g_first_eip;
static volatile uintptr_t g_first_addr;

/* Transparency-texture probe (--probe-blend). The shimmering wall's translucency
 * comes from its TEXTURE being a transparency-type DAS image (not a face flag),
 * so we gate on the TEXTURE having 0x80 texels — face-flag-independent (the prior
 * face-flag gates missed this wall, whose TEXTURE_FLAGS=0x04).
 * - 0x368a3 `mov esi,[esi]`: esi -> texture descriptor Q (+0xa flags, +0xc width,
 *   +0xe height, +0x10 pixels). Scan pixels for 0x80; if present it's a
 *   transparency texture -> set g_world_transp, count, OR its Q+0xa classification.
 * - blend writers (es/fs <- 0x490be2): count reaches, split by g_world_transp.
 * probe[0]=transp-texture world spans, probe[1]=those reaching a blend writer,
 * probe[2]=OR of their Q+0xa, probe[3]=total blend-writer reaches (any).
 * probe[0]>0 && probe[1]==0 => transparency-texture world spans NEVER blend
 * (systematic writer-select bug); probe[2] shows their classification bits. */
#define PROBE_DEREF_SITE  0x368a3u  /* mov esi,[esi] (2 bytes) -> Q descriptor */
/* blend writers: fs 0x2dc27, es cluster 0x385f9..0x3b023 (all `mov sr,[0x490be2]` 7B) */
const uint32_t g_blend_writer_sites[] = {
    0x2dc27,
    0x385f9, 0x38664, 0x390ad, 0x391d1, 0x392cd, 0x39399, 0x39454,
    0x39521, 0x3a101, 0x3a701, 0x3ae22, 0x3ae95, 0x3b023,
};
const int g_blend_writer_n = (int)(sizeof g_blend_writer_sites / sizeof g_blend_writer_sites[0]);
static volatile int g_cur_transp;   /* current span draws a transparency texture */
static volatile int g_cur_blended;  /* current span reached a blend writer */
static volatile uint32_t g_cur_qa;  /* current span's texture Q+0xa */
static void handler_body(cpu_t *c, siginfo_t *si);
static int irq_return(cpu_t *c);
static void gdv_publish_frame(void); /* publish the complete decoded cutscene frame */
extern volatile int g_gdv_emit_lifted; /* lift_registry.c: gdv_emit_decoded_frame is live-swapped */

/* GDV loop-hosting: when a lifted gdv_decode_frame bridges the multi-frame playback loop, the GDV
 * timer ISR that paces frame decode is suspended inside the trap. While g_gdv_loop_hosting is set,
 * shm_tick (SIGALRM, ~70 Hz like the real int-8 ISR) stands in for that ISR's per-tick decode by
 * calling the lifted codec + emit/advance directly. (Definitions in renderer.c.) */
extern volatile int g_gdv_loop_hosting;
void gdv_decode_video_chunk(void);   /* 0x4d384 — LZ/RLE codec (all fmts lifted) */
void gdv_advance_chunk_ptr(void);    /* 0x4dd33 — emit (publish+blit) + advance decoder head [d68]/[d6c] */
/* GDV decode-pump timer-ISR bodies (carved engine; frameless). The shm_tick surrogate IS the host timer
 * driver that calls them: decode_timer_isr (audio + pump), _noaudio (silent), tick (fade/read pump). */
void gdv_decode_timer_isr(void);          /* 0x4e60b */
void gdv_decode_timer_isr_noaudio(void);  /* 0x4e24b */
void gdv_tick_timer_isr(void);            /* 0x4e2ed */
void gdv_settle_palette_fade(void);       /* 0x4d2e0 — per-frame palette settler (blitter tail-call) */
void vsync_timer_tick(void);              /* 0x122e3 (the imgfree gameplay heartbeat body) */

/* Gameplay interactive-lift surrogate: near-call the frozen heartbeat (vsync_timer_tick 0x122e3) so
 * movement/cursor/clock advance under a loop-tier live-swap. Reentrant-safe. Defined in renderer.c. */
void host_run_isr_heartbeat(void);
/* OPT-IN (ROTH_LOOP_AUDIO=1) audio under the swap: far-call the frozen MIDI sequencer step
 * (sos_sequence_timer_tick 0x51ad5) per active track. Default off. (SFX-under-swap deferred.) renderer.c. */
void host_step_midi_tracks(void);

static void handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    /* The game runs with its own fs/gs selectors; the i386 kernel delivers
     * signals WITHOUT restoring the host's, and glibc needs gs for TLS.
     * Swap host segments in before touching any libc, restore on exit
     * (sigreturn then reloads the game's from the sigcontext). */
    uint16_t game_fs, game_gs;
    __asm__ volatile("mov %%fs, %0" : "=r"(game_fs));
    __asm__ volatile("mov %%gs, %0" : "=r"(game_gs));
    __asm__ volatile("mov %0, %%fs" :: "r"((uint32_t)g_host_fs));
    __asm__ volatile("mov %0, %%gs" :: "r"((uint32_t)g_host_gs));

    cpu_t cpu = {.uc = (ucontext_t *)ucv};
    cpu_t *c = &cpu;
    uint32_t eip = R_EIP(c);

    if (g_in_handler) {
        /* A nested fault while already servicing one. If it lands in GAME code (OBJ1), it is a serviceable
         * request the original is making under a differential's call_orig — a DOS int 0x21 / DPMI int 0x31 /
         * port I/O / cli-sti / leftover int3 (lift int3s are already suspended during call_orig). The most
         * common case: a render differential whose bridged render lazily LOADS an uncached texture (int 0x21
         * file I/O in the DAS loader). Serve it reentrantly via handler_body — the service handlers are
         * reentrant for these, and the original run is uncorrupted (it executes before any C lift). Only a
         * fault in HOST code (the C lift / libc, e.g. 0x80xxxxx) is a real recursive bug. A depth cap is the
         * runaway safety net.
         *
         * The audio HMI dispatch MAGIC pages (0xe0d1xxxx..0xe0d4xxxx = MAGIC_OFF/POLL/AFTER/MIDI in audio.c)
         * are the OTHER serviceable nested fault: a bridged restore that plays audio/MIDI (e.g. a savegame
         * lift's process_audio_sequence_chunk, reached via call_orig) far-calls a MAGIC pointer, which faults
         * OUTSIDE OBJ1. Route those to handler_body -> audio_trap too, or the load aborts as a "recursive
         * fault" (this was the load_savegame_file crash at MAGIC_MIDI 0xe0d40000). */
        static volatile int g_nest_depth = 0;
        int audio_magic = (eip >= 0xe0d10000u && eip < 0xe0d50000u);
        /* DIAGNOSTIC: localize the FIRST few nested bad far-calls (eip outside OBJ1 & not audio-magic).
         * For a far-call fault [game_esp]=return EIP (instruction AFTER the bad `call`, i.e. in the
         * CALLER) and [game_esp+4]=return CS. Log the canon caller VA so we know which game fn far-calls
         * garbage. (Fires before the gate, so it catches eip=0/0x80/buffer-addr cascades.) */
        if (!(eip >= OBJ1_BASE && eip < OBJ1_BASE + OBJ1_SIZE) && !audio_magic) {
            static int dbg_bad = 0;
            uint32_t gesp = R_ESP(c);
            uint32_t r0 = 0, r1 = 0;
            if (gesp >= OBJ3_BASE && gesp + 8 < STACK_TOP) {
                r0 = ((const uint32_t *)(uintptr_t)gesp)[0];
                r1 = ((const uint32_t *)(uintptr_t)gesp)[1];
            }
            if (dbg_bad++ < 8)
                LOGE("[gdv-badcall] #%d eip=0x%x esp=0x%x ret_eip=0x%x (canon 0x%x) ret_cs=0x%x depth=%d\n",
                     dbg_bad, eip, gesp, r0, (unsigned)(r0 - OBJ_DELTA), r1, g_nest_depth);
        }
        /* A far-call landing below the EXE image (eip < 0x1000) is a null/garbage MIDI/SOS DRIVER pointer:
         * audio_trap skips it with a far_ret (see audio.c "null audio callback guard"). It happens nested
         * too: a lift bridging an audio teardown (e.g. decode_frame's audio_stop_voice 0x55640 -> SOS
         * stop-voice 0x4ac55, which `lgs`-loads voice-descriptor far pointers and far-calls them) at a
         * SILENT cutscene's end far-calls null SOS handlers (seen: eip=0 then eip=0x80, successive null
         * driver fields). Route those to handler_body->audio_trap like the top level, else the recursive cap
         * fires. Legit protected-mode far-calls in this game always target the EXE objects (>=0x410000) or
         * DOS memory, never linear <0x1000, so this is safe. */
        if (((eip >= OBJ1_BASE && eip < OBJ1_BASE + OBJ1_SIZE) || audio_magic || eip < 0x1000) && g_nest_depth < 16) {
            g_nest_depth++;
            handler_body(c, si);
            g_nest_depth--;
            return;   /* sigreturn restores the nested frame's segregs/regs */
        }
        /* A lift (host C) faulted while dereferencing geometry through a raw selector-BASE pointer. The
         * original's equivalent access is bounded by the segment LIMIT (serviceable #GP), but the lift's flat
         * pointer SEGVs — this happens on garbage/transient input (e.g. a spurious int3 during a map load, or a
         * stale selector window). Recover: longjmp back to the differential, which skips this sample. */
        extern volatile int g_os_fault_armed;
        extern sigjmp_buf g_os_fault_jmp;
        if (g_os_fault_armed) {
            g_os_fault_armed = 0;
            siglongjmp(g_os_fault_jmp, 1);
        }
        extern volatile int g_wd_dbg_phase;
        char buf[384];
        /* Dump the stack top: for a far-call to a garbage pointer, [esp]=return EIP (the instruction
         * AFTER the bad `call`, i.e. inside the CALLER) and [esp+4]=return CS — this localizes which
         * game function made the call. Subtract OBJ_DELTA to read the return EIP as a canon VA. */
        uint32_t esp = R_ESP(c);
        uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        if (esp >= OBJ3_BASE && esp + 16 < STACK_TOP) {
            const uint32_t *st = (const uint32_t *)(uintptr_t)esp;
            s0 = st[0]; s1 = st[1]; s2 = st[2]; s3 = st[3];
        }
        int n = snprintf(buf, sizeof buf,
                         "[host] RECURSIVE fault in handler: eip=0x%x addr=%p (host-code fault or nest cap)\n"
                         "       first fault: eip=0x%x addr=0x%lx  wd_bridge_phase=%d\n"
                         "       caller stack @esp=0x%x: ret_eip=0x%x (canon 0x%x) ret_cs=0x%x +%08x %08x\n",
                         eip, si->si_addr, (unsigned)g_first_eip,
                         (unsigned long)g_first_addr, g_wd_dbg_phase,
                         esp, s0, (unsigned)(s0 - OBJ_DELTA), s1, s2, s3);
        write(2, buf, (size_t)n);
        _exit(6);
    }
    g_in_handler = 1;
    g_first_eip = eip;
    g_first_addr = (uintptr_t)si->si_addr;
    handler_body(c, si);
    g_in_handler = 0;

    /* Do NOT manually restore fs/gs here: sigreturn reloads the game's segment
     * registers from the ucontext (uc_mcontext.gregs[REG_FS/REG_GS]). A manual
     * `mov fs, game_fs` can itself #GP (e.g. if the faulting instruction was a
     * segment load) and cascade into a spurious "fault outside game code". */
    (void)game_fs;
    (void)game_gs;
}

static void handler_body(cpu_t *c, siginfo_t *si)
{
    uint32_t eip = R_EIP(c);

    calltrace_poll();   /* service pending ROTH_TRACE arm/dump (safe trap context) */
    capture_poll();     /* service pending ROTH_CAPTURE arm (safe trap context) */

    if (irq_return(c))
        return;

    /* Audio: virtual HMI driver dispatch + planted hooks (see audio.c). */
    if (audio_trap(c))
        return;

    if (eip < OBJ1_BASE || eip >= OBJ1_BASE + OBJ1_SIZE) {
        LOGE("fault outside game code (eip=0x%x addr=%p) — host bug?\n", eip,
             si->si_addr);
        uint32_t esp = R_ESP(c);
        if (esp >= OBJ3_BASE && esp < STACK_TOP) {
            const uint32_t *st = (const uint32_t *)esp;
            LOGE("stack: %08x %08x %08x %08x %08x %08x\n", st[0], st[1], st[2],
                 st[3], st[4], st[5]);
            LOGE("regs: eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n",
                 R_EAX(c), R_EBX(c), R_ECX(c), R_EDX(c), R_ESI(c), R_EDI(c));
        }
        signal(SIGSEGV, SIG_DFL);
        return; /* re-fault with default action for a core dump */
    }

    /* Quit-to-DOS clean-exit fix — the SOS-audio-driver-close/teardown DPMI far-call thunks. Several tiny
     * thunks (0x4fe44 / 0x4ff4c / … , from sos_driver_close_voices 0x48666 & siblings) marshal a
     * far-call to the SOS driver with the identical body:
     *     lfs   edi,[ebp+0x10]   ; 0f b4 7d 10  — load the far-pointer arg (selector:offset) into FS:EDI
     *     lcall *[ebp+8]         ; ff 5d 08     — far-call the driver (MAGIC-dispatched by audio.c; eax=fn#)
     *     pop es; pop gs; pop fs; pop ecx; pop ebx; pop edi; pop esi; pop ebp; ret   ; balanced epilogue
     * At program exit the far-pointer arg holds a real-mode/DPMI selector that no longer maps to a host
     * descriptor (the SOS driver is host-EMULATED — the real buffer free is done elsewhere), so the `lfs`
     * #GPs (SIGSEGV, addr=nil) and the host has nothing to do here. Detect the exact 7-byte lfs+lcall pattern
     * at the faulting eip and skip both to the balanced pop/ret (the skipped lfs only sets EDI/FS, both among
     * the popped regs). This fires ONLY when the lfs actually faults (an invalid selector = teardown), so the
     * normal in-play path where the arg selector is valid is untouched — and it covers every thunk instance
     * without hard-coding addresses. Net: a clean driver-close return -> clean Quit-to-DOS. */
    {
        const uint8_t *ip = (const uint8_t *)(uintptr_t)eip;
        if (ip[0] == 0x0f && ip[1] == 0xb4 && ip[2] == 0x7d && ip[3] == 0x10 &&  /* lfs edi,[ebp+0x10] */
            ip[4] == 0xff && ip[5] == 0x5d && ip[6] == 0x08) {                   /* lcall *[ebp+8]      */
            R_EIP(c) = eip + 7;   /* -> the `pop es` (0x07) that begins the balanced epilogue */
            return;
        }
    }

    /* GDV cutscene frame boundary (int3 planted at GDV_EMIT_SITE in main.c):
     * the decode buffer is now a COMPLETE frame about to be blitted — snapshot
     * it here so we never publish a half-decoded frame. Then emulate the
     * clobbered `cmp dword [g_gdv_user_callback], 0` (7 bytes) and resume. */
    if (si->si_signo == SIGTRAP && eip - 1 == GDV_EMIT_SITE + OBJ_DELTA &&
        *(uint8_t *)(uintptr_t)(eip - 1) == 0xcc && !g_gdv_emit_lifted) {
        /* g_gdv_emit_lifted: when gdv_emit_decoded_frame is live-swapped, DEFER to lift_dispatch
         * below — the lifted emit publishes itself via host_gdv_publish_frame. Default 0 keeps this
         * publish-only hook byte-for-byte identical for every non-emit-lift run. */
        gdv_publish_frame();
        uint32_t v = *(uint32_t *)(uintptr_t)(0x91d00u + OBJ_DELTA); /* g_gdv_user_callback */
        uint32_t fl = R_EFL(c) & ~0x8d5u;   /* clear CF,PF,AF,ZF,SF,OF */
        if (v == 0) fl |= 1u << 6;          /* ZF */
        if (v & 0x80000000u) fl |= 1u << 7; /* SF */
        uint8_t lb = (uint8_t)v, par = 1;
        for (int k = 0; k < 8; k++) par ^= (lb >> k) & 1;
        if (par) fl |= 1u << 2;             /* PF (even parity) */
        R_EFL(c) = fl;
        R_EIP(c) = (eip - 1) + 7;           /* past the cmp */
        return;
    }

    /* Live-swapped lifts (ROTH_LIFT): if this int3 is a registered lifted
     * function's entry, run the verified C reimplementation, simulate its ret,
     * and resume. lift_dispatch returns 0 for any non-lift fault. */
    if (lift_dispatch(c))
        return;

    /* Runtime call-trace (ROTH_TRACE): first-hit coverage for the active capture
     * window. Returns 0 for any non-trace int3. */
    if (calltrace_dispatch(c))
        return;

    /* Trace-replay capture (ROTH_CAPTURE): snapshot the entry state of the target
     * driver for offline replay verification. Returns 0 for any non-capture int3. */
    if (capture_dispatch(c))
        return;

    /* INT3 reach-probes (--probe-blend). The kernel delivers SIGTRAP with eip
     * pointing to the byte AFTER the 0xcc, so match target+1 (the opcode at eip
     * is NOT 0xcc — it's the 2nd byte of the original insn) and re-run the
     * original instruction whose first byte we clobbered. */
    if (g_probe_blend) {
        uint32_t i3 = eip - 1; /* address of the int3 byte */
        if (i3 == PROBE_DEREF_SITE + OBJ_DELTA) {  /* mov esi,[esi] -> Q descriptor */
            uint32_t rec = *(uint32_t *)(uintptr_t)R_ESI(c);
            R_ESI(c) = rec;
            R_EIP(c) = i3 + 2;
            /* finalize the PREVIOUS span: if it was a transparency texture that
             * did NOT reach a blend writer, sample its classification (the wall). */
            if (g_shm && g_cur_transp && !g_cur_blended)
                g_shm->probe[2] = g_cur_qa;        /* last NON-blend transp Q+0xa */
            /* start the new span */
            uint16_t w = *(uint16_t *)(uintptr_t)(rec + 0xc);
            uint16_t h = *(uint16_t *)(uintptr_t)(rec + 0xe);
            uint32_t total = (uint32_t)w * h;
            uint32_t n = total < 512u ? total : 512u;
            const uint8_t *px = (const uint8_t *)(uintptr_t)(rec + 0x10);
            int n80 = 0;
            for (uint32_t k = 0; k < n; k++) if (px[k] & 0x80) n80++;
            g_cur_qa = *(uint16_t *)(uintptr_t)(rec + 0xa);
            g_cur_transp = (n80 > 0);
            g_cur_blended = 0;
            if (g_shm && n80 > 0) g_shm->probe[0]++;
            if (g_force_blend && n80 > 0)
                *(uint16_t *)(uintptr_t)(rec + 0xa) |= g_force_mask;
            /* ROTH_DUMP_SURF: per distinct surface (deduped, reset each second), dump the texture's type
             * bits (rec+0xa), w/h, punch-out (==0) and blend (>=0x80) pixel counts, and the live draw-flags
             * 0x9093c. This is the 0x366cb path that the special (animated/portal) surfaces actually use, so
             * it captures them (unlike the 0x36b39-gated dump). Compare the portal's line vs the wall's. */
            static int s_dumpsurf = -1;
            if (s_dumpsurf < 0) s_dumpsurf = getenv("ROTH_DUMP_SURF") != NULL;
            if (s_dumpsurf) {
                int nz = 0; for (uint32_t k = 0; k < n; k++) if (!px[k]) nz++;
                uint16_t qa    = *(uint16_t *)(uintptr_t)(rec + 0xa);
                uint16_t flags = *(uint16_t *)(uintptr_t)(0x9093cu + OBJ_DELTA);
                static long last_sec = -1; static uint32_t seen[256]; static int nseen = 0;
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                if (ts.tv_sec != last_sec) { last_sec = ts.tv_sec; nseen = 0; }
                uint32_t sig = (uint32_t)qa ^ ((uint32_t)w << 16) ^ ((uint32_t)h << 24)
                             ^ ((uint32_t)flags << 3) ^ (nz ? 0x40000000u : 0u) ^ (n80 ? 0x80000000u : 0u);
                int dup = 0; for (int s = 0; s < nseen; s++) if (seen[s] == sig) { dup = 1; break; }
                if (!dup && nseen < 256) {
                    seen[nseen++] = sig;
                    char b[200]; int bk = snprintf(b, sizeof b,
                        "[surf] rec+0xa=%04x 9093c=%04x %ux%u  punch(==0)=%d/%u  blend(>=0x80)=%d/%u\n",
                        qa, flags, w, h, nz, n, n80, n);
                    write(2, b, (size_t)bk);
                }
            }
            return;
        }
        for (int i = 0; i < g_blend_writer_n; i++) {
            if (i3 == g_blend_writer_sites[i] + OBJ_DELTA) { /* mov es/fs,[0x490be2] */
                uint16_t sel = *(uint16_t *)(uintptr_t)(0x90be2u + OBJ_DELTA);
                if (g_blend_writer_sites[i] == 0x2dc27u)
                    c->uc->uc_mcontext.gregs[REG_FS] = sel;
                else
                    c->uc->uc_mcontext.gregs[REG_ES] = sel;
                R_EIP(c) = i3 + 7;
                if (g_cur_transp) {
                    g_cur_blended = 1;
                    if (g_shm) { g_shm->probe[1]++; g_shm->probe[3] = g_cur_qa; }
                }
                return;
            }
        }
    }

    /* Leftover debug breakpoints (int3 / 0xcc) baked into the game's OWN shipped code. ROTH.EXE ships at
     * least one on a live path (0x36981, on the blend-enable path inside the span classifier 0x366cb) — on
     * real DOS the DOS/4GW default INT3 handler simply resumes, so the breakpoint is a no-op there. Our own
     * tooling int3s (lift / trace / capture / --probe-blend) were already consumed by the dispatch checks
     * above and returned; anything still here that is a SIGTRAP landing one byte past a 0xcc inside OBJ1 code
     * is a game breakpoint. Emulate DOS/4GW: resume at eip (treat the int3 as a NOP) instead of falling into
     * the SIGSEGV opcode-emulator below (which would mis-handle the following instruction). The old default
     * of bailing on an "unexpected int3" was wrong for a game that ships leftover breakpoints. */
    if (si->si_signo == SIGTRAP) {
        uint32_t i3 = eip - 1;   /* the 0xcc byte (kernel reports eip past it) */
        if (i3 >= OBJ1_BASE && i3 < OBJ1_BASE + OBJ1_SIZE && *(uint8_t *)(uintptr_t)i3 == 0xcc) {
            static uint32_t seen[32]; static int nseen = 0; int dup = 0;
            for (int k = 0; k < nseen; k++) if (seen[k] == i3) { dup = 1; break; }
            if (!dup && nseen < 32) { seen[nseen++] = i3;
                LOGE("[host] leftover in-game int3 @canon 0x%x -> resume (DOS/4GW NOP)\n", i3 - OBJ_DELTA); }
            return;   /* resume at eip -> the instruction after the breakpoint runs normally */
        }
    }

    uint8_t *p = (uint8_t *)eip;
    int opsize = 4; /* 32-bit code segment default */
    int rep = 0;
    uint32_t len = 0;

    for (;;) { /* prefix scan */
        uint8_t b = p[len];
        if (b == 0x66) { opsize = 2; len++; continue; }
        if (b == 0xf2 || b == 0xf3) { rep = 1; len++; continue; }
        if (b == 0x67 || b == 0x2e || b == 0x36 || b == 0x3e || b == 0x26 ||
            b == 0x64 || b == 0x65) { len++; continue; }
        break;
    }
    uint8_t op = p[len];

    switch (op) {
    case 0xcc: /* int3 — handled above (probes); anything else is unexpected */
        dump_and_die(c, "unexpected int3");
        return;
    case 0xcd: /* int imm8 */
        g_trap_counts[0]++;
        R_EIP(c) = eip + len + 2;
        dispatch_int(c, p[len + 1]);
        return;
    case 0xfa: /* cli */
        g_trap_counts[3]++;
        g_soft_if = 0;
        R_EIP(c) = eip + len + 1;
        return;
    case 0xfb: /* sti */
        g_trap_counts[3]++;
        g_soft_if = 1;
        R_EIP(c) = eip + len + 1;
        return;
    case 0xf4: /* hlt — used as "wait"; just continue */
        LOGT("hlt at 0x%x\n", eip);
        R_EIP(c) = eip + len + 1;
        return;
    case 0xe4: g_trap_counts[1]++; port_in(c, p[len + 1], 1); R_EIP(c) = eip + len + 2; return;
    case 0xe5: g_trap_counts[1]++; port_in(c, p[len + 1], opsize); R_EIP(c) = eip + len + 2; return;
    case 0xe6: g_trap_counts[2]++; port_out(c, p[len + 1], 1); R_EIP(c) = eip + len + 2; return;
    case 0xe7: g_trap_counts[2]++; port_out(c, p[len + 1], opsize); R_EIP(c) = eip + len + 2; return;
    case 0xec: g_trap_counts[1]++; port_in(c, R_EDX(c) & 0xffff, 1); R_EIP(c) = eip + len + 1; return;
    case 0xed: g_trap_counts[1]++; port_in(c, R_EDX(c) & 0xffff, opsize); R_EIP(c) = eip + len + 1; return;
    case 0xee:
        g_trap_counts[2]++;
        if (palette_fastpath(c)) return; /* sets EIP+regs itself */
        porthist_add(eip);
        port_out(c, R_EDX(c) & 0xffff, 1); R_EIP(c) = eip + len + 1; return;
    case 0xef: g_trap_counts[2]++; port_out(c, R_EDX(c) & 0xffff, opsize); R_EIP(c) = eip + len + 1; return;
    case 0x6c: case 0x6d: case 0x6e: case 0x6f: /* ins/outs */
        LOGE("string port I/O at 0x%x (rep=%d) — skipping, ecx zeroed\n", eip, rep);
        if (rep)
            R_ECX(c) = 0;
        R_EIP(c) = eip + len + 1;
        return;
    default: {
        if ((uintptr_t)si->si_addr < 0x10000 &&
            emulate_lowmem(c, p + len, opsize, len)) {
            g_trap_counts[4]++;
            return;
        }
        g_trap_counts[5]++;
        char msg[96];
        snprintf(msg, sizeof msg,
                 "unhandled fault (op 0x%02x, addr %p) — likely memory access",
                 op, si->si_addr);
        dump_and_die(c, msg);
    }
    }
}

/* ------------------------------------------------------ IRQ injection -- */

volatile int g_soft_if = 1;
volatile int g_in_irq;
uint32_t g_irq_eip; /* game EIP the last timer IRQ preempted (audio.c reads it) */

/* audio-timer quiesce (cluster-7 timer-event-table fence). See roth_host.h for the full contract.
 * g_au_timer_locked: nesting depth of the future host-C audio-timer natives' table edit; nonzero =
 * locked (inject_irq defers int-8). Written ONLY by au_timer_lock/unlock, which run in the game's
 * normal execution (single-threaded); read by inject_irq in SIGALRM context. The transitions bracket
 * a coherent table (lock before the edit, unlock after), so a SIGALRM racing the non-atomic ++/-- is
 * safe: it sees the old-or-new value, and the table is coherent on both sides of each transition.
 * g_au_timer_irq_pending: a single int-8 was deferred while locked; alarm_handler delivers it on the
 * first SIGALRM after the lock clears. Both are 0 until a native calls au_timer_lock() (none does
 * yet), so the paths below that test them are dead and the timer path is byte-identical to today. */
volatile int g_au_timer_locked;
static volatile int g_au_timer_irq_pending;

void au_timer_lock(void)   { g_au_timer_locked++; }
void au_timer_unlock(void) { if (g_au_timer_locked > 0) g_au_timer_locked--; }

/* imgfree-boot ISR mode — the always-interactive, no-inject_irq SIGALRM path. Set ONLY
 * by the future ROTH_STANDALONE_BOOT=1 boot (roth_boot()); NOTHING assigns it nonzero yet, so every
 * branch below that tests it is DEAD and the normal trap-host is behaviourally unchanged (the
 * quiesce precedent: a flag nothing sets = provable dead code). When the mode IS on:
 *   - alarm_handler forces g_os_interactive=1 so shm_tick runs the surrogate (frame clock 0x90bcc, the
 *     lifted GDV accumulator pump, cursor, audio_tick poll, iso_apply_scancode keyboard) and then RETURNS
 *     before the inject_irq decision — obj1 is unmapped image-free, so there is no g_pm_vec handler to
 *     jump into and the SOS master-timer table walk 0x49eaf is bypassed (its title-relevant effects, the
 *     frame clock + MIDI, come from the surrogate / the audio ladder). The gameplay heartbeat
 *     host_run_isr_heartbeat and MIDI host_step_midi_tracks are LIVE-SWAP hooks (they call_orig into
 *     obj1) → NOT usable image-free: the menu bare-bump path is forced (see shm_tick), and title MIDI is
 *     the audio ladder's job (honest STOP).
 *   - au_timer quiesce interaction: vacuously satisfied. The lock defers inject_irq(0x08) to keep the
 *     ORIGINAL int-8 ISR (0x49eaf) from sampling a half-written SOS timer table; image-free never calls
 *     inject_irq, so g_au_timer_irq_pending is never set and no tick is ever deferred. The surrogate's own
 *     C timer work (0x90bcc bump + gdv accumulators) does not read that table, so it is NOT lock-gated —
 *     matching how the existing live-swap surrogate's heartbeat already runs regardless of the lock (only
 *     inject_irq(0x08) is deferred). Moot in practice: g_au_timer_locked is always 0 (no native locks). */
volatile int g_standalone_boot;

/* DOS/4GW reflects hardware IRQs to AH=25-installed PM handlers via an
 * INTERRUPT frame: the handler ends in `iretd`. We push EFLAGS/CS/EIP exactly
 * like the CPU would and vector to the handler; its native iretd pops the
 * frame and resumes the interrupted code with no host involvement on the
 * return path. (Handlers that instead chain to the AH=35 "old vector" land on
 * IRQ_RET_MAGIC, where irq_return emulates the iretd as a safety net.) */
static void inject_irq(ucontext_t *uc, uint8_t vec)
{
    /* audio-timer quiesce: while a host-C audio-timer native holds the SOS timer-event-table lock,
     * DEFER (do not drop) the master-timer int-8 — its ISR 0x49eaf reads that very table, so injecting
     * it mid-edit would sample a torn table. Remember the tick; alarm_handler re-injects it the moment
     * the lock clears. Timer-IRQ only: keyboard (int-9) and every other vector fall straight through,
     * so they keep flowing during the (microsecond) edit window. Dead unless a native locks. */
    if (vec == 0x08 && g_au_timer_locked) {
        g_au_timer_irq_pending = 1;
        return;
    }
    uint32_t handler_va = g_pm_vec_int21[vec];
    uint32_t eip = (uint32_t)uc->uc_mcontext.gregs[REG_EIP];
    if (!handler_va || g_in_handler || !g_soft_if || g_in_irq) {
        /* task #15 trace: a keyboard IRQ that can't inject leaves its scancode(s) stuck in g_kbd_q
         * (see the interactive-path reclaim). Count/log the gate that blocked it. */
        if (vec == 0x09 && kbd_trace_on()) {
            const char *why;
            if (!handler_va)      { why = "no_handler";           g_irq9_supp_other++; }
            else if (g_in_handler){ why = "in_handler(dispatch)"; g_irq9_supp_inhandler++; }
            else if (!g_soft_if)  { why = "cli(soft_if=0)";       g_irq9_supp_softif++; }
            else                  { why = "in_irq";               g_irq9_supp_inirq++; }
            static int n; if (n++ < 80)
                LOGE("[kbd] IRQ9 suppressed: %s -> scancode stuck in 8042 queue frame=%u\n",
                     why, g_shm ? g_shm->frame : 0);
        }
        return;
    }
    if (eip < OBJ1_BASE || eip >= OBJ1_BASE + OBJ1_SIZE)
        return; /* interrupted host/libc code: skip this tick */

    uint32_t esp = (uint32_t)uc->uc_mcontext.gregs[REG_ESP];
    if (esp < OBJ3_BASE + 0x1000 || esp > STACK_TOP)
        return; /* unexpected stack; don't make it worse */

    LOGT("irq%d inject at eip=0x%x\n", vec == 8 ? 0 : 1, eip);
    uint32_t *st = (uint32_t *)(esp - 12);
    st[2] = (uint32_t)uc->uc_mcontext.gregs[REG_EFL];
    st[1] = (uint32_t)uc->uc_mcontext.gregs[REG_CS];
    st[0] = eip;
    uc->uc_mcontext.gregs[REG_ESP] = (greg_t)(esp - 12);
    uc->uc_mcontext.gregs[REG_EIP] = (greg_t)handler_va;
    g_irq_eip = eip; /* the game code this IRQ preempted (for audio.c gating) */
    g_in_irq = 1;
    g_irq_active++;
}

/* Safety net: a handler chained to the magic old-vector instead of iretd.
 * Emulate iretd by popping the interrupt frame ourselves. */
static int irq_return(cpu_t *c)
{
    if (R_EIP(c) != IRQ_RET_MAGIC)
        return 0;
    uint32_t esp = R_ESP(c);
    const uint32_t *st = (const uint32_t *)esp;
    R_EIP(c) = st[0];
    R_EFL(c) = st[2];
    R_ESP(c) = esp + 12;
    if (g_irq_active > 0 && --g_irq_active == 0)
        g_in_irq = 0;
    return 1;
}

/* ---- VESA hi-res (mode 0x101, 640x480x256, banked) --------------------
 * The game renders to the 0xA0000 64K window and pages through the 640x480
 * image with int10 4F05. We keep a full 640x480 backing buffer and copy the
 * window <-> the active bank on each switch, so the window always mirrors the
 * current bank. At present time we flush the current bank and ship the whole
 * image. (Copy-on-switch is a few hundred KB/frame — negligible.) */
int g_hires;
static uint8_t g_hires_fb[ROTH_FB_MAXW * ROTH_FB_MAXH];
static int g_cur_bank;
#define HIRES_BYTES (ROTH_FB_MAXW * ROTH_FB_MAXH)

static void hires_flush_window(void) /* window (0xA0000) -> backing[cur_bank] */
{
    uint32_t off = (uint32_t)g_cur_bank * 0x10000u;
    if (off >= HIRES_BYTES)
        return;
    uint32_t n = HIRES_BYTES - off;
    if (n > 0x10000u)
        n = 0x10000u;
    memcpy(g_hires_fb + off, (const void *)(uintptr_t)VGA_LIN, n);
}

void vesa_set_mode(int hires)
{
    g_hires = hires;
    g_cur_bank = 0;
    if (hires) {
        memset(g_hires_fb, 0, sizeof g_hires_fb);
        memset((void *)(uintptr_t)VGA_LIN, 0, 0x10000);
    }
}

int vesa_get_bank(void) { return g_cur_bank; }

void vesa_set_bank(int bank)
{
    if (!g_hires || bank == g_cur_bank)
        return;
    hires_flush_window();                 /* save outgoing bank */
    g_cur_bank = bank;
    uint32_t off = (uint32_t)bank * 0x10000u; /* load incoming bank */
    if (off < HIRES_BYTES) {
        uint32_t n = HIRES_BYTES - off;
        if (n > 0x10000u) n = 0x10000u;
        memcpy((void *)(uintptr_t)VGA_LIN, g_hires_fb + off, n);
    }
}

/* Read a game global by its canonical (Ghidra) address: runtime = canon +
 * OBJ_DELTA. Safe from the timer handler — the model is flat, so absolute
 * linear reads work regardless of the active DS. */
#define GV32(canon) (*(volatile uint32_t *)(uintptr_t)((canon) + OBJ_DELTA))
#define GV16(canon) (*(volatile uint16_t *)(uintptr_t)((canon) + OBJ_DELTA))
#define GV8(canon)  (*(volatile uint8_t  *)(uintptr_t)((canon) + OBJ_DELTA))

/* Byte offset into the VESA LFB of the page the game last set on-screen (VBE
 * 4F07 display start = scanline*pitch + x). Tracked so the inspect-popup publish
 * can ship the page actually being displayed — the engine draws the popup's
 * choice/subtitle text straight to the LFB display, not the back buffer the
 * normal present snapshots. Updated by vesa_set_display_start (from dpmi.c). */
static uint32_t g_vesa_disp_off;

void vesa_set_display_start(uint32_t scanline, uint32_t x)
{
    g_vesa_disp_off = scanline * (uint32_t)GV16(VA_g_screen_pitch) + x; /* x g_screen_pitch */
}

/* Plain mode 13h = the only display path that reaches the screen as a linear
 * 0xA0000 image the host can read straight back: no VESA linear FB, no banked
 * VESA, and the engine's raw-screen flag set. Everything else ("Mode-X" and the
 * VESA modes) is a double-buffered, planar/banked present we publish from the
 * engine's linear back buffer instead (see video_publish_composed). */
static int video_is_vga13h(void)
{
    return GV32(VA_g_linear_framebuffer_ptr) == 0  /* g_linear_framebuffer_ptr */
        && GV16(VA_g_video_linear_flag) == 0  /* g_video_linear_flag */
        && GV8(VA_g_rawscreen_flag) != 0;  /* g_rawscreen_flag */
}

/* The mouse cursor is a save-under sprite the game draws onto the (planar/
 * banked) video target AFTER the present, so it never reaches the linear back
 * buffer we publish — composite it ourselves. The sprite (g_cursor_sprite) is
 * w@0x7e950, h@0x7e951, pixels@0x7e952 with a fixed 0x12-byte row stride and
 * value 0 = transparent; it draws at top-left (0x76880,0x76884) and is hidden
 * during mouselook. Line-doubled modes (320x400) draw the cursor at 2x height.
 * Env ROTH_NO_CURSOR disables this. */
static void composite_cursor(uint8_t *fb, uint16_t pitch, uint16_t ph)
{
    static int off = -1;
    if (off < 0) off = getenv("ROTH_NO_CURSOR") ? 1 : 0;
    if (off) return;
    if (GV8(VA_g_mouse_relative_mode)) return;                  /* g_mouse_relative_mode (mouselook) */
    uint8_t cw = GV8(VA_g_cursor_sprite), ch = GV8(VA_g_cursor_sprite + 0x1);
    if (cw == 0 || ch == 0 || cw > 32 || ch > 32)
        return;
    int32_t cx = (int32_t)GV32(VA_g_saveunder_sprite_color_ptr + 0x8);       /* cursor top-left x (logical) */
    int32_t cy = (int32_t)GV32(VA_g_saveunder_sprite_color_ptr + 0xc);       /* cursor top-left y (logical) */
    int ys = GV8(VA_g_hires_line_doubling_flag) ? 2 : 1;             /* g_hires_line_doubling_flag */
    cy *= ys;
    const uint8_t *spr = (const uint8_t *)(uintptr_t)(0x7e952u + OBJ_DELTA);
    for (int sy = 0; sy < ch; sy++) {
        for (int r = 0; r < ys; r++) {
            int py = cy + sy * ys + r;
            if (py < 0 || py >= ph) continue;
            for (int sx = 0; sx < cw; sx++) {
                uint8_t v = spr[sy * cw + sx];     /* row stride == width */
                if (v == 0) continue;              /* transparent */
                int px = cx + sx;
                if (px < 0 || px >= pitch) continue;
                fb[(uint32_t)py * pitch + px] = v;
            }
        }
    }
}

/* Publish the engine's linear back buffer (g_framebuffer_ptr) for the buffered
 * modes. Called at the PAGE FLIP — VESA int10 4F07 and the Mode-X CRTC start-
 * address write — because by then the renderer has finished the frame (the
 * flip follows the present). Publishing on the async 70 Hz timer instead caught
 * the buffer mid-render, which is the "geometry flickers through walls" bug.
 * Runs in the game's trap context; the model is flat so the copies are safe. */
/* The last complete scene (cursor-free), snapshotted at the flip. shm_tick
 * re-publishes it every timer tick and re-draws the cursor on top, so the
 * cursor stays smooth even in static menus that flip rarely (without re-reading
 * the engine's buffer mid-render, which would flicker). */
static uint8_t g_clean_frame[ROTH_FB_MAX];
static uint32_t g_clean_w, g_clean_h, g_clean_ah;
static int g_clean_valid;

#ifdef ROTH_STANDALONE
/* Single-pass overlay-publish staging (item-grabber "scanlines" fix). The SDL2 viewer
 * (viewer.c, a separate process) reads g_shm->pixels UNSYNCHRONIZED — the shared_fb.h frame
 * seqlock was designed but never consumed. If the mod overlay were composed directly onto
 * g_shm->pixels, its panel region would momentarily hold the bare world laid down by the
 * per-tick publish memcpy (before the compose pass paints the overlay), so an async viewer
 * read landing between the two writes captures gameworld-through-the-panel horizontal bands
 * (intermittent; worst in hires where the panel is large and frameless). Instead the inline
 * publish paths assemble the FULL frame (world + game cursor + overlay + overlay cursor) into
 * this private buffer, and shm_tick blits it to g_shm->pixels in ONE memcpy: the panel region
 * then transitions overlay(N-1) -> overlay(N) with no bare-world in between, so even a torn read
 * shows overlay-over-world in both halves. Static (this runs in the SIGALRM tick — no stack
 * buffer, no allocation); sized by ROTH_FB_MAX. */
static uint8_t g_pub_stage[ROTH_FB_MAX];
#endif

/* Publish the on-screen VESA linear-framebuffer page directly (NOT the back
 * buffer). Used during the inventory inspect popup, whose choice list + bottom
 * subtitle are drawn straight to the LFB display — they never reach the back
 * buffer video_publish_composed snapshots, so that path shows the window but no
 * text (confirmed: while a dialogue plays the back-buffer band-sum is frozen
 * while the LFB pages change). The displayed page is g_vesa_disp_off (4F07).
 * Returns 1 if it published, 0 if not in LFB mode (Mode-X handled elsewhere). */
static int video_publish_lfb_page(void)
{
    uint32_t lfb = GV32(VA_g_linear_framebuffer_ptr);           /* g_linear_framebuffer_ptr */
    if (lfb < VESA_LFB_LIN)
        return 0;                           /* not a (mapped) LFB mode */
    uint16_t pitch = GV16(VA_g_screen_pitch), ph = GV16(VA_g_screen_height);
    if (pitch < 1 || pitch > ROTH_FB_MAXW || ph < 1 || ph > ROTH_FB_MAXH ||
        (uint32_t)pitch * ph > ROTH_FB_MAX)
        return 0;
    uint32_t off = g_vesa_disp_off;
    if (lfb + off + (uint32_t)pitch * ph > VESA_LFB_LIN + VESA_LFB_SIZE)
        off = 0;                            /* stale/out-of-range start -> page 0 */
    memcpy(g_shm->pixels, (const void *)(uintptr_t)(lfb + off), (uint32_t)pitch * ph);
    memcpy(g_shm->palette, g_dac_rgb, sizeof g_shm->palette);
    g_shm->cur_w = g_shm->aspect_w = pitch;
    g_shm->cur_h = ph;
    /* line-doubled modes display at half height (logical 4:3); same as composed */
    g_shm->aspect_h = GV8(VA_g_hires_line_doubling_flag) ? (uint32_t)ph / 2 : ph;
    /* The displayed page already carries the game's own cursor (drawn to the
     * video target), so do NOT composite the host cursor on top here. */
    return 1;
}

void video_publish_composed(void)
{
    if (video_is_vga13h())
        return;
    uint32_t fb    = GV32(VA_g_framebuffer_ptr); /* g_framebuffer_ptr (linear back buffer) */
    uint16_t pitch = GV16(VA_g_screen_pitch); /* g_screen_pitch (row stride == width) */
    uint16_t ph    = GV16(VA_g_screen_height); /* g_screen_height (physical scanlines) */
    if (pitch < 1 || pitch > ROTH_FB_MAXW || ph < 1 || ph > ROTH_FB_MAXH ||
        (uint32_t)pitch * ph > ROTH_FB_MAX || fb < DOSMEM_LIN ||
        fb + (uint32_t)pitch * ph > DOSMEM_LIN + DOSMEM_SIZE)
        return;
    memcpy(g_clean_frame, (const void *)(uintptr_t)fb, (uint32_t)pitch * ph);
    g_clean_w = pitch;
    g_clean_h = ph;
    /* Line-doubled modes (320x400) carry 2x the scanlines but should display at
     * their logical aspect (320x200) — a 4:3-ish screen, not a tall portrait. */
    g_clean_ah = GV8(VA_g_hires_line_doubling_flag) ? (uint32_t)ph / 2 : ph; /* g_hires_line_doubling_flag */
    g_clean_valid = 1;
}

/* modex_arm — faithful transcription of the Mode-X arm 0x2e36b (shared by the image-free
 * flip/blank hooks): [0x90be4]=1; if the LFB is active or the raw-screen flag is set it backs off
 * to 0xffff; otherwise the 0x2e390 tail RESETS the four page words ({a000,4000,a400,0}, or the
 * [0x90be6]&4 320x400 double-scan variant {a000,8000,a800,0}). The tail's VGA unchain port
 * sequence is I/O the trap host drops too (port_out ignores 0x3c4/0x3ce and CRTC idx != 0x0c), so
 * these memory writes are its whole trap-observable contract. */
static void modex_arm(void)
{
    GV16(VA_g_vga_mode_configured) = 1;
    if (GV32(VA_g_linear_framebuffer_ptr) != 0 || GV8(VA_g_rawscreen_flag) != 0) { GV16(VA_g_vga_mode_configured) = 0xffff; return; }
    if (GV16(VA_g_video_mode_flags) & 4) {
        GV16(VA_g_init_stage_error_strings + 0x134) = 0xa000; GV16(VA_g_init_stage_error_strings + 0x136) = 0x8000;
        GV16(VA_g_init_stage_error_strings + 0x138) = 0xa800; GV16(VA_g_init_stage_error_strings + 0x13a) = 0;
    } else {
        GV16(VA_g_init_stage_error_strings + 0x134) = 0xa000; GV16(VA_g_init_stage_error_strings + 0x136) = 0x4000;
        GV16(VA_g_init_stage_error_strings + 0x138) = 0xa400; GV16(VA_g_init_stage_error_strings + 0x13a) = 0;
    }
}

/* host_flip_video_page — the image-free host present for flip_video_page 0x2e1e8 (host_video_driver
 * klass, NO lifted body). The trap host runs the ORIGINAL bytes: it programs the CRTC start address
 * (Mode-X) or issues int10 4F07 (VESA LFB), and the host presents the engine back buffer at that flip
 * (video_publish_composed / the int10 trap). Image-free there is no original code and no port/int trap,
 * so this reproduces the SAME observable effects: the faithful double-buffer page-global swaps of
 * 0x2e1e8 (minus the moot hardware vsync-wait) + the host present. What the PPM byte-diff sees is
 * g_shm->pixels, which shm_tick feeds from the snapshot taken here (buffered modes) or from the VGA
 * window (13h — a no-op-safe path here). eax carries the original's mode/page arg (only the vsync
 * timing bits, which the host has no hardware for). */
void host_flip_video_page(uint32_t eax)
{
    (void)eax;
    uint32_t lfb = GV32(VA_g_linear_framebuffer_ptr);                  /* g_linear_framebuffer_ptr (0 = Mode-X/VGA planar) */
    if (lfb != 0) {                                /* VESA LFB branch (0x2e2bb): int10 4F07 set-display-start */
        uint32_t scan = GV16(VA_g_init_stage_error_strings + 0x13a) ? (uint32_t)GV16(VA_g_screen_height) : 0;
        vesa_set_display_start(scan, 0);
        uint16_t a = GV16(VA_g_init_stage_error_strings + 0x136); GV16(VA_g_init_stage_error_strings + 0x136) = GV16(VA_g_init_stage_error_strings + 0x13a); GV16(VA_g_init_stage_error_strings + 0x13a) = a;
    } else {
        if (GV8(VA_g_rawscreen_flag) != 0) return;             /* 0x2e2ba: flip suppressed */
        uint16_t a = GV16(VA_g_init_stage_error_strings + 0x136); GV16(VA_g_init_stage_error_strings + 0x136) = GV16(VA_g_init_stage_error_strings + 0x13a); GV16(VA_g_init_stage_error_strings + 0x13a) = a;
        if (GV16(VA_g_vga_mode_configured) != 1) modex_arm();       /* 0x2e23c: call 0x2e36b (arms + page-word reset) */
    }
    video_publish_composed();                      /* the CRTC/4F07 flip -> snapshot the back buffer */
    GV16(VA_g_render_target_buffer + 0xc) = GV16(VA_g_frame_tick_counter);                 /* 0x2e292: record the frame counter after the flip */
    { uint16_t a = GV16(VA_g_init_stage_error_strings + 0x134); GV16(VA_g_init_stage_error_strings + 0x134) = GV16(VA_g_init_stage_error_strings + 0x138); GV16(VA_g_init_stage_error_strings + 0x138) = a; }
}

/* modex_clear_draw_page — 0x2e18b: sequencer map-mask=0xf (port I/O the trap host drops), then
 * rep-stosd zero of the DRAW page in the VGA window at [0x71f04]<<4 (0xa0000/0xa4000...), one
 * Mode-X page = 0xfa0 dwords, doubled under the line-doubling flag [0x90cbd]. The memory clear is
 * the whole trap-observable effect. */
static void modex_clear_draw_page(void)
{
    uint32_t off = (uint32_t)GV16(VA_g_init_stage_error_strings + 0x134) << 4;
    uint32_t n = 0xfa0u * 4;
    if (GV8(VA_g_hires_line_doubling_flag) != 0) n *= 2;
    memset((void *)(uintptr_t)off, 0, n);
}

/* host_blank_active_video_page — the image-free host body for blank_active_video_page 0x2e140
 * (host_video_driver, NO lifted body): nothing under VESA-linear [0x76634] or an LFB [0x146d8];
 * arm Mode-X if not armed (0x2e15c: call 0x2e36b); raw-screen 13h [0x90c08] -> clear the whole
 * 0xA0000 window (rep stosd 0x3e80 dwords = 64000 B); else clear-draw-page / flip(0) /
 * clear-draw-page so BOTH pages end blank and the flip publishes the black frame. */
void host_blank_active_video_page(void)
{
    if (GV32(VA_g_video_linear_flag) != 0) return;
    if (GV32(VA_g_linear_framebuffer_ptr) != 0) return;
    if (GV16(VA_g_vga_mode_configured) == 0) modex_arm();
    if (GV8(VA_g_rawscreen_flag) != 0) {
        memset((void *)(uintptr_t)VGA_LIN, 0, 0xfa00);
        return;
    }
    modex_clear_draw_page();
    host_flip_video_page(0);
    modex_clear_draw_page();
}

/* Publish a COMPLETE decoded GDV cutscene frame. Called from the SIGTRAP hook
 * at gdv_emit_decoded_frame (GDV_EMIT_SITE) — the buffer is fully decoded there,
 * unlike the async timer which caught it mid-write. Per
 * docs/reference/ROTH_gdv_format_notes.md: the frame is a SINGLE LINEAR 8-bpp
 * image at DAT_91d40 (planar interleave is at the VGA dest, not the source);
 * width/height from the header (DAT_91d44 +0x14/+0x16), stride = DAT_91ca8,
 * palette = the live DAC. Half-res frames (DAT_91de2) are doubled to the full
 * display size, matching the blitter. */
static void gdv_publish_frame(void)
{
    if (!g_shm)
        return;
    /* The inventory "inspect" popup plays a GDV into a small MODAL WINDOW, not
     * full-screen. load_dbase300_resource_at_offset (0x196b9) forces a 320x200
     * Mode-X context with its OWN per-frame user callback (0x18e09) that records
     * the decoded frame and returns 1 ("presented"); the codec therefore does NO
     * hardware blit, and the op-2 frame-boundary handler composites each frame
     * into the popup window of the game back buffer (0x13183 + add_dirty_rect)
     * and page-flips. Publishing the raw decode buffer here would blow that
     * window up to fill the screen — the reported bug. The popup is flagged by
     * g_inspect_popup_active (0x7fec0, written ONLY by 0x196b9), so bail and let
     * the normal composed publish (video_publish_composed) show the windowed
     * frame. Full-screen cutscenes never set this flag.
     *
     * The decode loop composites the popup window into the back buffer but only
     * page-flips conditionally, so don't rely on the flip hook to refresh the
     * snapshot: this int3 fires on every decoded frame, so refresh the composed
     * back buffer here and let shm_tick publish it per tick. */
    if (GV32(VA_g_inspect_popup_active)) {               /* g_inspect_popup_active */
        video_publish_composed();      /* snapshot back buffer (with the window) */
        return;
    }
    uint32_t base = GV32(VA_g_gdv_decode_buffer + 0xc);     /* linear 8-bpp decode buffer */
    uint32_t hdr  = GV32(VA_g_gdv_decode_buffer + 0x10);     /* GDV frame header */
    if (base < DOSMEM_LIN || hdr < DOSMEM_LIN)
        return;
    int w = *(const uint16_t *)(uintptr_t)(hdr + 0x14);  /* display width  */
    int h = *(const uint16_t *)(uintptr_t)(hdr + 0x16);  /* display height */
    int stride = (int16_t)GV16(VA_g_dpcm_step_table + 0x404); /* source row stride (==w for 8-bpp) */
    if (w < 1 || w > ROTH_FB_MAXW || h < 1 || h > ROTH_FB_MAXH ||
        (uint32_t)w * h > ROTH_FB_MAX || stride < w || stride >= 2 * w)
        return;                        /* bad dims / 16-bpp hi-colour */
    /* Per-frame dynamic half-res (DAT_91de2): the GDV interleaves full-res and
     * half-res frames. A half-res frame fills only the top half of the rows
     * (0x08 = vertical) and/or left half of the columns (0x04 = horizontal) of
     * the buffer; the real blitter DOUBLES those to fill the full display. We do
     * the same — without it, a half-res frame shows new content in the top/left
     * and stale pixels in the rest. */
    uint8_t de2 = GV8(VA_g_gdv_end_of_stream + 0x4);
    int vh = (de2 & 0x08) ? 1 : 0;     /* vertical half-res this frame   */
    int hh = (de2 & 0x04) ? 1 : 0;     /* horizontal half-res this frame */
    if (g_video_log) {
        uint32_t ctx = GV32(VA_g_gdv_context);
        uint32_t sflags = ctx >= DOSMEM_LIN ? *(const uint32_t *)(uintptr_t)(ctx + 0x18) : 0;
        static uint32_t pkey, p_sflags;
        uint32_t key = (uint32_t)w | ((uint32_t)h << 12) | ((uint32_t)de2 << 24);
        if (key != pkey || sflags != p_sflags) {
            LOGE("gdv: hdr=%dx%d stride=%d de2=0x%02x (vh=%d hh=%d) sflags=0x%x\n",
                 w, h, stride, de2, vh, hh, sflags);
            pkey = key; p_sflags = sflags;
        }
        static int geom_once;
        if (!geom_once) { geom_once = 1;
            LOGE("gdv-geom: cec=%d cf0=%d ce8=%d off870=%d da0=%d flags=0x%x dcf=0x%02x "
                 "dce=0x%04x lfb=0x%x ca8=%d\n",
                 (int)GV32(VA_g_dpcm_step_table + 0x448), (int)GV32(VA_g_dpcm_step_table + 0x44c), (int)GV32(VA_g_dpcm_step_table + 0x444),
                 (int)GV32(VA_g_particle_pool + 0xc), (int)GV32(VA_g_gdv_audio_stream_base + 0x50), (unsigned)GV32(VA_g_gdv_stream_flags),
                 GV8(VA_g_gdv_audio_format + 0x5), GV16(VA_g_gdv_audio_format + 0x4), (unsigned)GV32(VA_g_linear_framebuffer_ptr), (int)GV16(VA_g_dpcm_step_table + 0x404));
        }
    }
    const uint8_t *src = (const uint8_t *)(uintptr_t)base;

    /* --- Place the frame INTO the mode-sized display canvas, centered exactly as
     * gdv_blit_frame_to_vga (0x4c7a5) does — do NOT stretch it to fill the window.
     *
     * The original blits the native w x h frame into a mode-W x mode-H surface
     * (DAT_91cec x DAT_91cf0), display-scaled 2x horizontally when
     * g_gdv_stream_flags & 0x2000 and 2x vertically when & 0x10020 (doubling),
     * at the centered offset DAT_91870 = (mode_W-disp_w)/2 + (mode_H-disp_h)/2 *
     * pitch (gdv_init_frame_geometry 0x4bf4c lines computing L20/L1c). The
     * surround stays the surface's cleared black (index 0). Publishing the raw
     * w x h frame as the WHOLE display (the old behaviour) let the viewer stretch
     * a 640x280-in-640x480 letterbox up to fill the window — the reported vertical
     * stretch. Reproduce the surface instead: a mode-sized black canvas with the
     * frame at its true position, so the live viewer AND the SIGTERM PPM dump show
     * the same 4:3 image with letterbox bars the original produced.
     *
     * Verified on the intro (GREMLOGO/INSTR*): VESA-LFB 640x480, native 320x280,
     * flags 0x40a190 (0x2000 set) -> displayed 640x280, DAT_91870=64000 =>
     * (x0=0, y0=100) == (640-640)/2, (480-280)/2. */
    uint32_t flags = GV32(VA_g_gdv_stream_flags);        /* g_gdv_stream_flags */
    int MW  = (int)GV32(VA_g_dpcm_step_table + 0x448);          /* mode display width  (canvas) */
    int MH  = (int)GV32(VA_g_dpcm_step_table + 0x44c);          /* mode display height (canvas) */
    int hdbl = (flags & 0x2000)  ? 1 : 0;  /* horizontal 2x display scale */
    int vdbl = (flags & 0x10020) ? 1 : 0;  /* vertical   2x display scale */
    int dispW = w << hdbl, dispH = h << vdbl;
    int hshift = hdbl + hh;                /* source-col downshift (display + de2) */
    int vshift = vdbl + vh;                /* source-row downshift (display + de2) */

    /* The centering math is mode-independent and equals DAT_91870 decoded for
     * a linear/LFB surface (and unlike DAT_91870 it is not the Mode-X planar /4
     * offset) — so it is correct for both linear and Mode-X full-screen cutscenes. */
    int canvas_ok = MW >= dispW && MW >= 1 && MH >= 1 &&
                    MW <= ROTH_FB_MAXW && MH <= ROTH_FB_MAXH &&
                    (int64_t)MW * MH <= ROTH_FB_MAX;
    if (canvas_ok) {
        int x0 = (MW - dispW) / 2, y0 = (MH - dispH) / 2;
        int sx = 0, sy = 0;                /* source crop for a frame taller/wider than the mode */
        if (x0 < 0) { sx = -x0; x0 = 0; }
        if (y0 < 0) { sy = -y0; y0 = 0; }
        int cw = dispW - sx; if (cw > MW - x0) cw = MW - x0; if (cw < 0) cw = 0;
        int ch = dispH - sy; if (ch > MH - y0) ch = MH - y0; if (ch < 0) ch = 0;
        uint8_t *dst = g_shm->pixels;
        memset(dst, 0, (size_t)MW * MH);   /* letterbox/pillarbox bars = index 0 (cleared surface) */
        for (int i = 0; i < ch; i++) {
            const uint8_t *srow = src + (size_t)((sy + i) >> vshift) * stride;
            uint8_t *drow = dst + (size_t)(y0 + i) * MW + x0;
            if (hshift == 0)
                memcpy(drow, srow + sx, (size_t)cw);
            else
                for (int j = 0; j < cw; j++)
                    drow[j] = srow[(sx + j) >> hshift];
        }
        memcpy(g_shm->palette, g_dac_rgb, sizeof g_shm->palette);
        g_shm->cur_w = g_shm->aspect_w = (uint32_t)MW;
        g_shm->cur_h = (uint32_t)MH;
        /* line-doubled (320x400) modes display at their logical 4:3 aspect */
        g_shm->aspect_h = GV8(VA_g_hires_line_doubling_flag) ? (uint32_t)MH / 2 : (uint32_t)MH;
        g_shm->frame++;
        return;
    }

    /* Fallback (mode geometry not a sane canvas, e.g. before gdv_setup_video_mode
     * ran): publish the native frame directly — a valid, if window-filling, image. */
    uint8_t *dst = g_shm->pixels;
    for (uint32_t dy = 0; dy < (uint32_t)h; dy++) {
        const uint8_t *srow = src + (size_t)(dy >> vh) * stride; /* V-double if vh */
        uint8_t *drow = dst + (size_t)dy * w;
        if (!hh)
            memcpy(drow, srow, (size_t)w);
        else
            for (uint32_t dx = 0; dx < (uint32_t)w; dx++)
                drow[dx] = srow[dx >> 1];                       /* H-double */
    }
    memcpy(g_shm->palette, g_dac_rgb, sizeof g_shm->palette);
    g_shm->cur_w = g_shm->aspect_w = (uint32_t)w;
    g_shm->cur_h = g_shm->aspect_h = (uint32_t)h;
    g_shm->frame++;
}

/* Public entry for the lifted gdv_emit_decoded_frame (lift_gdv_cutscene.c): publish the complete
 * decoded frame exactly as the GDV_EMIT_SITE int3 hook does. When emit is live-swapped the host's
 * entry-int3 publish branch is gated off (g_gdv_emit_lifted), and the lift reproduces the publish via
 * this hook (wired to g_os_publish_frame in lift_install). */
void host_gdv_publish_frame(void)
{
    gdv_publish_frame();

#if defined(ROTH_STANDALONE)
    /* task #106 FIX (image-free only): the lifted image-free GDV frame blitter (gdv_blit_frame_to_vga /
     * _alt 0x4c7a5/0x4c788, gdv_cutscene.c) reproduces the blitter body's data side-effects but DROPS the
     * original 2840-byte blitter's `jmp 0x4d2e0` tail-call into gdv_settle_palette_fade. So image-free the
     * per-frame palette settle/restore never runs: after a GDV fmt-1 DAC fade (the intro white flash) the
     * new scene's palette is never re-uploaded and the scene renders with the stale flash palette (the
     * "next scene colours are corrupted / different palette" bug). The trap-host oracle runs the original
     * blitter bytes (settle fires), so this only affects the image-free lane.
     * This publish seam runs once per emitted frame (g_os_publish_frame in gdv_emit_decoded_frame) — the
     * same cadence as the dropped blitter tail — so drive the settler here, gated exactly like the emit
     * blitter's 0x4c788 dispatch: it reaches its 0x4d2e0 tail on every path EXCEPT the three early rets
     * (format byte [0x91dc0]: bit7 set, or low-nibble 0 or 3). Verified: the image-free intro palette
     * sequence now matches the oracle byte-for-byte through both flashes (blank+restore pairs restored).
     * ROTH_GDV_PAL_FIX_OFF=1 reverts to the pre-fix behaviour for A/B comparison. */
    static int fix = -1;
    if (fix < 0) fix = getenv("ROTH_GDV_PAL_FIX_OFF") ? 0 : 1;   /* ON by default; OFF to A/B-revert */
    if (fix) {
        uint8_t al = GV8(VA_g_gdv_audio_stream_base + 0x70);     /* [0x91dc0] format byte */
        if (!(al & 0x80) && (al & 0xf) != 0 && (al & 0xf) != 3)
            gdv_settle_palette_fade();                           /* the dropped 0x4c788 -> 0x4d2e0 tail */
    }
#endif
}

/* shm_tick gate: while a cutscene runs, the int3 hook (gdv_publish_frame) owns
 * the framebuffer — tell shm_tick to skip its own publish. */
static int video_publish_gdv(void)
{
    /* True only for a FULL-SCREEN GDV (cutscene). The inventory inspect popup is
     * a windowed GDV composited into the back buffer and shown via the normal
     * composed publish, so it must NOT suppress shm_tick's per-tick publish.
     * See gdv_publish_frame for the full rationale. */
    return GV8(VA_g_gdv_decoder_active) != 0          /* g_gdv_decoder_active */
        && GV32(VA_g_inspect_popup_active) == 0;        /* ...but NOT the windowed inspect popup */
}

/* Apply ONE viewer scancode exactly as keyboard_int9_isr 0x12393 would (the host int-9 stand-in used
 * while g_os_interactive freezes the real ISR): fold LShift(0x2a)->RShift(0x36), drop the E0/E1
 * extended prefixes, maintain the held-key bitmap g_key_state_bitmap @0x90c3c (make sets, break
 * clears — this is what dispatch_held_key_actions 0x128fb reads for movement), and enqueue non-
 * modifier makes into the menu input ring @0x90c1c. Shared by the shm-ring drain AND the 8042-queue
 * reclaim (task #15). Returns 0=make, 1=break, 2=prefix-dropped (for ROTH_KBD_TRACE labelling only).
 * Transcribed byte-for-byte from keyboard_int9_isr; see lift_input.c. */
static int iso_apply_scancode(uint8_t sc)
{
#if defined(ROTH_STANDALONE) && !defined(ROTH_VANILLA)
    sc = plugins_dispatch_scancode(sc);        /* plugin on_scancode chain (task #103); the only input seam now */
    if (sc == 0)                               /* 0 = swallowed (overlay input capture); no state change, */
        return 2;                              /*     like a dropped prefix */
#endif
    uint8_t idx = sc & 0x7f;                   /* make-index (break bit cleared) = bitmap byte selector */
    uint8_t raw = sc;                          /* raw scancode; low 3 bits select the bit mask */
    if (idx == 0x2a) {                         /* fold left-shift into right-shift (fake-shift) */
        idx = 0x36;
        raw = (uint8_t)((sc & 0x80) | 0x36);
        sc  = raw;
    }
    if (sc == 0xe0 || sc == 0xe1)              /* extended prefix: no state change */
        return 2;
    uint8_t bit = *(volatile uint8_t *)(uintptr_t)(0x707e9u + OBJ_DELTA + (raw & 7));
    volatile uint8_t *cell =
        (volatile uint8_t *)(uintptr_t)(0x90c3cu + OBJ_DELTA + (idx >> 3));
    if ((sc & 0x80) == 0) {                    /* make: set the held bit + enqueue (non-modifier keys) */
        *cell |= bit;
        if (sc != 0x38 && sc != 0x2a && sc != 0x36) {   /* skip alt / L-shift / R-shift */
            uint16_t mask = GV16(VA_g_saved_int9_segment + 0x4);
            uint16_t head = GV16(VA_g_saved_int9_segment + 0x6);
            uint16_t next = (uint16_t)((head + 1) & mask);
            if (next != GV16(VA_g_saved_int9_segment + 0x8)) {       /* not full (else drop, like a HW overrun) */
                *(volatile uint8_t *)(uintptr_t)(0x90c1cu + OBJ_DELTA + head) = sc;
                GV16(VA_g_saved_int9_segment + 0x6) = next;
            }
        }
        return 0;
    }
    *cell &= (uint8_t)~bit;                     /* break: clear the held bit */
    return 1;
}

/* Publish the current framebuffer + palette to shared memory and pull pending
 * input from the viewer. Runs in the timer handler (host segments active). */
static void shm_tick(void)
{
    audio_tick(); /* diagnostic: is the SOS mixer rendering PCM? */
#ifdef ROTH_STANDALONE
    /* Image-free digital-audio pump (audio.c): the MAGIC_POLL body — SFX/speech mixer + done-cb
     * delivery + play-position advance — driven here per SIGALRM tick because the SOS master-timer
     * walk (0x49eaf) that far-calls the registered poll cb never runs image-free (inject_irq is
     * bypassed). No-op until the install stages the far-args segment (g_farg_base). */
    audio_standalone_tick();
#endif
    if (!g_shm)
        return;
    if (g_shm->quit) {
        sfx_trace_exit_dump();   /* SFX-dropout trace (ROTH_SFX_TRACE): flush before the window-close
                                  * hard _exit — the one exit path atexit/clean-return hooks miss.
                                  * Gated no-op when disarmed; runs on the pump thread (dump-safe). */
        _exit(0);
    }
    /* Hold the hidden developer flag (canon 0x7f560 bit0) enabled. Set every
     * tick so it survives the CRT's one-time BSS clear at startup; the enable
     * routine the devs left at 0x14464 has no xrefs, so this is the same net
     * effect as their own enable_dev_mode. Unlocks W=map-warp, L=maphack, etc. */
    if (g_devmode) {
        static int announced;
        if (!announced) {
            announced = 1;
            LOGE("dev mode enabled (W=map warp, L=maphack, F3=aim)\n");
        }
        *(volatile uint8_t *)(uintptr_t)(0x7f560u + OBJ_DELTA) |= 1;
    }
    memcpy(g_shm->palette, g_dac_rgb, sizeof g_shm->palette);

    /* Plain mode 13h reaches the screen as a linear 0xA0000 image (and carries
     * its own cursor) — publish it straight from the VGA window. The buffered
     * "Mode-X"/VESA modes publish the scene snapshot taken at the last page flip
     * (video_publish_composed) and re-draw the cursor here every tick, so the
     * pointer stays smooth in menus that flip rarely. */
    /* Overlay publish target: normally g_shm->pixels directly, but in ROTH_STANDALONE the inline
     * publish paths below build into g_pub_stage so the mod-overlay compose can be published in a
     * single memcpy (see g_pub_stage). The helper paths (GDV / LFB inspect-popup / corner-peek)
     * write g_shm->pixels themselves and leave ig_staging=0 -> compose runs directly, as today. */
    uint8_t *pubtgt = g_shm->pixels;
#ifdef ROTH_STANDALONE
    int ig_staging = 0;
#endif
    if (video_publish_gdv()) {
        /* cutscene frame published above */
    } else if (video_is_vga13h()) {
#ifdef ROTH_STANDALONE
        pubtgt = g_pub_stage; ig_staging = 1;   /* stage -> single-pass publish (overlay tear fix) */
#endif
        memcpy(pubtgt, (const void *)(uintptr_t)VGA_LIN,
               ROTH_FB_W * ROTH_FB_H);
        g_shm->cur_w = g_shm->aspect_w = ROTH_FB_W;
        g_shm->cur_h = g_shm->aspect_h = ROTH_FB_H;
    } else {
        /* Buffered (Mode-X / VESA) modes normally publish the snapshot taken at
         * the last page flip (video_publish_composed) — NOT a per-tick read — so
         * the world renderer is never caught mid-frame ("geometry through walls").
         *
         * The inventory inspect popup is the exception: its choice list + bottom
         * subtitle are drawn straight to the DISPLAY surface, not the back buffer
         * the snapshot reads, so they never appear (the window does, since it is
         * in the back buffer). The popup freezes the world, so it is safe to read
         * the live display here. In VESA LFB mode that means shipping the on-screen
         * LFB page (video_publish_lfb_page). Otherwise fall back to refreshing +
         * shipping the back buffer (covers non-LFB / Mode-X). Scoped to the popup
         * so the world-render snapshot path is untouched. */
        int done = 0;
        if (GV32(VA_g_inspect_popup_active)) {                 /* g_inspect_popup_active */
            done = video_publish_lfb_page();
            if (!done)
                video_publish_composed();    /* refresh back-buffer snapshot */
            /* KNOWN LIMITATION: in unchained Mode-X (no LFB) the popup text is
             * drawn to the planar 0xA0000 display, which the host neither reads
             * nor de-planarizes (Mode-X is shown via the chunky back buffer). So
             * the back-buffer fallback above shows the popup window but not the
             * choice list / subtitle. Fixing it needs real Mode-X plane emulation
             * (map-mask tracking + per-plane store + de-planarize). VESA works. */
        } else if (GV32(VA_g_corner_icon_saveunder + 0x4)) {          /* g_corner_peek_anim: top-left portrait easing/held */
            /* Same present-model gap for the corner peek-icon / character
             * portrait: per frame
             * gameplay_frame_step 0x1792c draws it into the back buffer,
             * flush_dirty_rects blits it to the DISPLAY, then restore_corner_
             * peek_icon ERASES it from the back buffer before flip_video_page —
             * so the flip snapshot never contains it; it lives only on the
             * display. While the anim counter 0x7fdc0 (ramped 0..0x19 by
             * run_gameplay_frame 0x1691c, zeroed by clear_corner_peek_icon) is
             * nonzero, ship the live LFB display page instead.
             * UNLIKE the popup there is NO composed-refresh fallback: gameplay
             * is LIVE here (the popup freezes the world), and a per-tick
             * back-buffer read would catch the world renderer mid-frame (the
             * "geometry through walls" tear). In non-LFB Mode-X we just fall
             * through to the normal flip snapshot: the widget stays invisible
             * there — the same planar-display blind spot as the popup above. */
            done = video_publish_lfb_page();
        }
        if (!done && g_clean_valid) {
#ifdef ROTH_STANDALONE
            pubtgt = g_pub_stage; ig_staging = 1;   /* stage -> single-pass publish (overlay tear fix) */
#endif
            memcpy(pubtgt, g_clean_frame, g_clean_w * g_clean_h);
            g_shm->cur_w = g_clean_w;
            g_shm->cur_h = g_clean_h;
            g_shm->aspect_w = g_clean_w;
            g_shm->aspect_h = g_clean_ah;
            composite_cursor(pubtgt, (uint16_t)g_clean_w, (uint16_t)g_clean_h);
        }
    }

    if (g_video_log) {
        static uint32_t p_fb, p_dim, p_v13 = 0xffffffffu;
        uint32_t fb = GV32(VA_g_framebuffer_ptr), v13 = video_is_vga13h();
        uint32_t dim = ((uint32_t)GV16(VA_g_screen_pitch) << 16) | GV16(VA_g_screen_height);
        if (fb != p_fb || dim != p_dim || v13 != p_v13) {
            LOGE("video: g_framebuffer_ptr=0x%x pitch=%u height=%u dbl=%u -> %s "
                 "(sel_mode=%d rawscreen=0x%x linear=0x%x mode_flags=0x%x fb_bytes=0x%x)\n",
                 fb, GV16(VA_g_screen_pitch), GV16(VA_g_screen_height), GV8(VA_g_render_double_scanline_flag),
                 v13 ? "vga13h" : "FLIP/back-buffer",
                 (int)GV32(VA_g_selected_video_mode), GV8(VA_g_rawscreen_flag), GV16(VA_g_video_linear_flag),
                 GV16(VA_g_video_mode_flags), GV32(VA_g_framebuffer_bytes));
            p_fb = fb; p_dim = dim; p_v13 = v13;
        }
    }
#ifdef ROTH_STANDALONE
    /* Plugin compose seam (on_compose_tick, TICK_ISR): draws onto the (staged) frame — after every
     * publish path above, before any presenter sees it — so it works uniformly in all video modes and
     * no game buffer is ever touched. Runs in the SIGALRM tick: a plugin may only composite pre-rendered
     * pixels here (no engine calls / DAS / allocation). No-op while no overlay is open.
     * When the frame was staged (inline publish paths, ig_staging), compose into g_pub_stage and then
     * blit the finished frame to g_shm->pixels in ONE memcpy so the async viewer never samples the panel
     * mid-transition (see g_pub_stage). frame++ stays LAST — after the pixels are fully published.
     * Compiled out in the vanilla flavor (-DROTH_VANILLA: no plugin loader linked). */
#ifndef ROTH_VANILLA
    plugins_dispatch_compose_tick(pubtgt, g_shm->cur_w, g_shm->cur_h);
#endif
    if (ig_staging) {
        uint32_t stage_n = g_shm->cur_w * g_shm->cur_h;
        if (stage_n && stage_n <= ROTH_FB_MAX)
            memcpy(g_shm->pixels, g_pub_stage, stage_n);
    }
#endif
    g_shm->frame++;
    if (g_probe_blend && (g_shm->frame % 140) == 0) /* ~once/2s */
        LOGE("probe: transpTex-spans=%u  reached-blend=%u  NONblend-qa=0x%x  blend-qa=0x%x\n",
             g_shm->probe[0], g_shm->probe[1], g_shm->probe[2], g_shm->probe[3]);
    if (kbd_trace_on() && (g_shm->frame % 140) == 0 &&
        (g_irq9_supp_inhandler | g_irq9_supp_inirq | g_irq9_supp_softif | g_irq9_supp_other)) /* task #15 */
        LOGE("[kbd] IRQ9-suppressed totals: in_handler=%lu in_irq=%lu cli=%lu other=%lu\n",
             g_irq9_supp_inhandler, g_irq9_supp_inirq, g_irq9_supp_softif, g_irq9_supp_other);

    /* Interactive-lift stand-in for the frozen int-8/int-9 ISRs (see g_os_interactive). The lifted
     * menu/prompt is running inside the trap dispatch with the game's IRQs suspended, so do here what
     * the two ISRs would: */
    if (g_os_interactive) {
        /* Is the live-swapped lift running actual GAMEPLAY (player movement enabled, byte[0x7674a]&1)? */
        int gameplay = (GV8(VA_g_player_movement_enabled) & 1) != 0;

        /* A GDV decode session is OPEN — a full-screen cutscene OR the inventory item-inspect popup's silent
         * GDV (draw-callback [0x91d00]). We host its frozen decode-pump regardless of the gameplay flag,
         * because the inspect popup keeps movement ENABLED (0x7674a&1 set) — so it lands in the gameplay
         * branch and its pump would otherwise never run ("inspect modal freezes on frame 0").
         * g_gdv_loop_hosting = a lifted gdv_decode_frame is driving playback; g_gdv_decoder_active
         * 0x91ddc = a cutscene/popup running as ORIGINAL bytes under a loop swap. Opt out with
         * ROTH_NO_GDV_UNDER_LOOP=1 (falls back to the interim ROTH_LIFT=game_play_loop,gdv). */
        static int gdv_no_r4 = -1;
        if (gdv_no_r4 < 0) gdv_no_r4 = getenv("ROTH_NO_GDV_UNDER_LOOP") ? 1 : 0;
        /* REQUIRE a real decode session (0x91ddc, strictly open->close bounded: set @0x4b770,
         * cleared @0x4b890/0x4b95f) in ALL cases: show_cutscene_playback_menu sets
         * g_gdv_loop_hosting=1 with NO session open, and the un-gated pump then re-decoded/shipped
         * the LAST GDV's audio from the freed stream buffer (stale audio at gallery-menu open,
         * garbage SAMPLESCAN desc, DAS-cache scribble). A live hosted playback always has the
         * session open, so this only removes the corpse-pump case. */
        int gdv_active = GV8(VA_g_gdv_decoder_active) != 0 && (g_gdv_loop_hosting || !gdv_no_r4);

        /* (1) Clock + heartbeat, downsampled to the native ~70 Hz — SIGALRM follows the PIT (MIDI can bump
         * it to 120 Hz), so a naive +1 per SIGALRM over-drove the game clock ~1.7x. On each
         * 70 Hz beat: in GAMEPLAY run the near-safe heartbeat body (host_run_isr_heartbeat -> vsync_timer_tick
         * 0x122e3 = 0x90bcc inc + cursor + player_movement_tick + the [0x7e8d4] hook) so movement advances;
         * otherwise (menus/prompts/cutscenes/inspect popup, movement disabled) just the bare 0x90bcc
         * bump. NOTE: game AUDIO under the swap is handled elsewhere — digital SFX are host-emulated (audio.c,
         * off the host output tick), and MIDI is deferred (its step 0x51ad5 is a FAR SOS event; 0x49eaf is
         * entangled with the DPMI reflection wrapper). See host_run_isr_heartbeat.
         * Running the heartbeat is safe even while a GDV decodes (the inspect popup keeps movement enabled) —
         * 0x122e3 doesn't touch the GDV pump, which is hosted independently below (menus/cutscenes keep the
         * validated bare-bump path). */
        {
            static double hb_sub;
            double hb_hz = g_pit_div ? 1193182.0 / (double)g_pit_div : 70.0;
            double hb_rate = 70.0 / hb_hz;
            if (hb_rate > 1.0) hb_rate = 1.0;     /* PIT slower than 70 Hz (startup) -> heartbeat is PIT-limited */
            hb_sub += hb_rate;
            if (hb_sub >= 1.0) {
                hb_sub -= 1.0;
#ifndef ROTH_STANDALONE
                if (gameplay && !g_standalone_boot) host_run_isr_heartbeat();  /* 0x122e3 @ 70 Hz: 0x90bcc + cursor + movement */
                else GV16(VA_g_frame_tick_counter) += 1;                 /* g_frame_tick_counter @ 70 Hz (menus/cutscenes; imgfree forces this bare bump — the heartbeat is a call_orig hook into obj1, so the image-free gameplay heartbeat = vsync_timer_tick is deferred) */
#else
                /* The "cannot move at all" fix: the old always-bare-bump deferral is
                 * CLOSED — in gameplay run the LIFTED heartbeat body directly (vsync_timer_tick 0x122e3
                 * = 0x90bcc inc + software cursor + player_movement_tick; pure C — its only bridge, the
                 * [0x7e8d4] frame-tick hook, has ZERO writers in the whole corpus, provably never
                 * installed). Menus/cutscenes keep the validated bare bump, mirroring the trap lane. */
                if (gameplay) vsync_timer_tick();
                else GV16(VA_g_frame_tick_counter) += 1;
#endif
            }
        }

        /* (1a) OPT-IN audio under the swap (ROTH_LOOP_AUDIO=1, default OFF). Both game MIDI and digital SFX
         * are frozen under a loop swap (their SOS service is timer-ISR-driven). Currently this
         * drives the MIDI half — the frozen MIDI sequencer step, per SIGALRM (its registered rate ≈ the
         * PIT/SIGALRM rate), in both gameplay and menus. Off the 70 Hz heartbeat cadence (MIDI is faster).
         * Kept opt-in because it far-calls the SOS/MIDI driver from the surrogate; the validated
         * movement path is unaffected when off. SFX-under-swap is DEFERRED (its audio_mix_sfx render needs
         * the trap-context done-callbacks to be useful); when built it
         * rides this same gate. See host_step_midi_tracks. */
#ifndef ROTH_STANDALONE   /* host_step_midi_tracks call_orig_far's into obj1 — compiled out image-free (title MIDI = audio ladder, STOP) */
        {
            static int loop_audio = -1;
            if (loop_audio < 0) loop_audio = getenv("ROTH_LOOP_AUDIO") ? 1 : 0;
            if (loop_audio) host_step_midi_tracks();
        }
#endif

        /* (1b/1c) The frozen GDV timer ISR's per-tick accumulator bumps — the fade accumulator [0x91dbc]
         * (gdv_fade_in/out spin on it) + the read-pacing budget [0x91884] (gdv_read_frame_chunk spins on it)
         * — are now produced by the LIFTED ISR bodies. This surrogate IS the host timer driver: it calls the
         * right lifted ISR per tick (gdv_tick_timer_isr 0x4e2ed for non-decode interactive lifts;
         * gdv_decode_timer_isr 0x4e60b / _noaudio 0x4e24b during loop-hosting — each does its own [dbc]/[884]
         * bumps + decode). See lift_gdv_cutscene.c "GDV decode-pump TIMER ISR bodies". */

        /* (1d) GDV LOOP-HOSTING: when a lifted gdv_decode_frame is driving the multi-frame playback
         * loop via a call_orig bridge (g_gdv_loop_hosting), the game's int-8 GDV timer ISR (0x4e60b)
         * is frozen inside the trap — but THAT ISR is the frame PACER: its audio-service (0x4e310)
         * decodes one chunk per audio-slot boundary and (via 0x4dd33) advances the decoder head
         * [0x91d68], which run_playback_loop spins waiting on. With the ISR frozen, the loop reads the
         * buffer full then spins forever (the "stuck on frame 0" symptom). Replicate that audio-service
         * here per SIGALRM tick (~70 Hz, the real ISR's rate) so frames advance. All decode work is
         * PURE LIFTED C — codec gdv_decode_video_chunk (0x4d384) + emit/advance
         * gdv_advance_chunk_ptr (0x4dd33); the only fault is emit's bridged Mode-X blit, serviced
         * nested exactly as when emit runs in the trap. This mirrors the real async int-8 ISR (whose
         * preemption of the loop the game already tolerates via the [0x91de0]/[0x91df4] guards). */
        static unsigned dbg_lh_ticks = 0, dbg_lh_fires = 0;
        static int dbg_lh_prev = 0, dbg_lh_log = -1;
        if (dbg_lh_log < 0) dbg_lh_log = getenv("ROTH_GDV_LH_LOG") ? 1 : 0;
        /* START/END edge log (low-volume: one line per cutscene). Shows whether decode_frame even
         * enters its loop for this cutscene + the entry state — incl. d00 (popup draw-callback ptr)
         * and ddc (decoder-active). If a cutscene plays with NO "loop-hosting START" line, decode_frame
         * skipped the loop (begin_playback CF) -> the visible-stall/popup-absent cases land here. */
        if (dbg_lh_log && gdv_active && !dbg_lh_prev)
            LOGE("[gdv-lh] START d0c=%#x d28=%u ddc=%u d00=%#x dba=%d hosting=%d (mode=%s)\n",
                 (unsigned)GV32(VA_g_gdv_stream_flags), (unsigned)GV32(VA_g_gdv_context + 0x14), GV8(VA_g_gdv_decoder_active),
                 (unsigned)GV32(VA_g_gdv_user_callback), (int)(int16_t)GV16(VA_g_gdv_audio_stream_base + 0x6a), (int)g_gdv_loop_hosting,
                 (GV32(VA_g_gdv_stream_flags) & 2) ? "timer" : ((GV32(VA_g_gdv_stream_flags) & 4) ? "streamed"
                     : (GV8(VA_g_sos_timer_event_count + 0x8) ? "audio-cb" : "no-audio")));
        if (dbg_lh_log && !gdv_active && dbg_lh_prev)
            LOGE("[gdv-lh] END dba=%d fires=%u\n", (int)(int16_t)GV16(VA_g_gdv_audio_stream_base + 0x6a), dbg_lh_fires);
        dbg_lh_prev = gdv_active;
        if (gdv_active) {
            if (dbg_lh_log && (dbg_lh_ticks++ % 35) == 0)   /* ~2x/sec, ROTH_GDV_LH_LOG only */
                LOGE("[gdv-lh] tick=%u d0c=%#x d28=%u db8=%d db4=%d d60=%#x d6c=%#x d68=%#x d70=%#x dba=%d fires=%u\n",
                     dbg_lh_ticks, (unsigned)GV32(VA_g_gdv_stream_flags), (unsigned)GV32(VA_g_gdv_context + 0x14),
                     (int)(int16_t)GV16(VA_g_gdv_audio_stream_base + 0x68), (int)(int16_t)GV16(VA_g_gdv_audio_stream_base + 0x64),
                     (unsigned)GV32(VA_g_gdv_audio_stream_base + 0x10), (unsigned)GV32(VA_g_gdv_audio_stream_base + 0x1c), (unsigned)GV32(VA_g_gdv_audio_stream_base + 0x18),
                     (unsigned)GV32(VA_g_gdv_audio_stream_base + 0x20), (int)(int16_t)GV16(VA_g_gdv_audio_stream_base + 0x6a), dbg_lh_fires);

            if (GV32(VA_g_gdv_stream_flags) & 2) {
                /* TIMER-PACED: the int-8 GDV timer ISR (0x4e60b, now lifted C) decodes via its pump
                 * (0x4e310) on the [0x91db0]/[0x91dbe] budget + bumps [dbc]/[884]. NOTE: the lifted pump is
                 * FAITHFUL to 0x4e310 (decode path does NOT refill [db0] — self-paced by the d60==d6c
                 * caught-up reset), vs the prior surrogate's refill approximation; needs a [d0c]&2 cutscene
                 * to validate the pacing (timer mode is rare; most cutscenes are audio-cb / no-audio). */
                gdv_decode_timer_isr();
                dbg_lh_fires++;
            } else if (GV8(VA_g_sos_timer_event_count + 0x8)) {
                /* AUDIO-CALLBACK-PACED mode (audio active, [0x91d0c]&2 clear — e.g. the INTRO): video
                 * decode is
                 * driven by run_playback_loop's drain (0x4dff4), gated on the pending-subframe count
                 * [0x91db4] that the SOS buffer-complete callback 0x4e394 produces. That callback rides
                 * MAGIC_POLL (a game far-call), frozen inside the lift. Stand in for its VIDEO effect
                 * (0x4e3e0): consume one audio frame + queue one video subframe + advance the
                 * audio-consumed cursor [0x91d70]; the loop's own drain then decodes the video IN THE
                 * TRAP (no signal-context codec). Pace one block per `period` ticks, the SAME rate the
                 * host audio stream-driver uses (audio.c): period = 70 * (samples/block) / sample_rate,
                 * samples/block = [0x91d28]>>1. (My earlier [0x91db0]/[0x91dbe] proxy was the timer-path
                 * budget -> ~2x too fast here.) Audio ADPCM/submit (call [0x91898] / 0x55620) is still
                 * skipped — cutscene plays silently for now. */
                /* The int-8 timer ISR still fires in audio-cb mode (no decode — [d0c]&2 clear), bumping
                 * [dbc] + the read-pacing budget [884] the loop's reads spin on. */
                gdv_decode_timer_isr();
                static double lh_acc;
                uint32_t spb = (uint32_t)GV32(VA_g_gdv_context + 0x14) >> 1;  /* samples per audio block */
                /* period in SIGALRM ticks = tick_hz * block_seconds. tick_hz follows the game's PIT
                 * (the host retunes setitimer to g_pit_div; MIDI bumps it 70->120 Hz in-game, which
                 * is why a fixed 70 doubled the speed once in-game). */
                double tick_hz = g_pit_div ? 1193182.0 / (double)g_pit_div : 70.0;
                double period = spb ? tick_hz * (double)spb / 22050.0 : 5.0; /* rate=22050 (default) */
                if (period < 1.0) period = 1.0;
                lh_acc += 1.0;
                if (lh_acc >= period) {
                    lh_acc -= period;
                    if ((int16_t)GV16(VA_g_gdv_audio_stream_base + 0x68) >= 0 && GV8(VA_g_gdv_audio_end) == 0) {
                        uint32_t d60 = GV32(VA_g_gdv_audio_stream_base + 0x10);
                        if (d60 == 0xffffffffu || d60 != (uint32_t)GV32(VA_g_gdv_audio_stream_base + 0x20)) {
#ifdef ROTH_STANDALONE
                            /* The AUDIO half of 0x4e394 this stand-in's bookkeeping omits: decode the
                             * block at [0x91d70] via the [0x91898] replica, queue it to the stream
                             * voice (0x55620) and ship the PCM to the ring (audio.c). Runs BEFORE the
                             * cursor advance below, exactly where the original decodes. The validated
                             * video bookkeeping/pacing is unchanged. */
                            audio_standalone_stream_ship();
#endif
                            GV16(VA_g_gdv_audio_stream_base + 0x68)--;             /* audio frames remaining-- */
                            GV16(VA_g_gdv_audio_stream_base + 0x64)++;             /* pending video subframes++ (drain consumes) */
                            /* advance [0x91d70] past this record (0x4e405 tail; stored runtime ptr) */
                            uint32_t a = (uint32_t)GV32(VA_g_gdv_audio_stream_base + 0x20) + (uint32_t)GV32(VA_g_gdv_context + 0x14);
                            uint16_t paysz = *(volatile uint16_t *)(uintptr_t)(a + 2);
                            a = (a + paysz + 0xf) & ~7u;
                            if (a + (uint32_t)GV32(VA_g_gdv_audio_stream_base + 0x4c) > (uint32_t)GV32(VA_g_gdv_audio_stream_base + 0xc))
                                a = (uint32_t)GV32(VA_g_gdv_audio_stream_base);  /* wrap */
                            GV32(VA_g_gdv_audio_stream_base + 0x20) = (int32_t)a;
                            dbg_lh_fires++;
                        }
                    }
                }
            } else {
                /* NO-AUDIO mode (audio inactive + not streamed — e.g. the inventory item-closeup MODAL
                 * popup, which plays a SILENT GDV via a draw-callback [0x91d00]). With no audio there is
                 * no SOS buffer-complete callback to pace decode; instead a dedicated SOS timer vector
                 * (handler 0x4e24b, installed at 0x555d3) decodes via decode_subframe 0x4dceb (= codec +
                 * emit/advance), paced by the [0x91db0]/[0x91dbe] budget and gated on the FRAME counter
                 * [0x91dba] (not the audio position). That timer is frozen in the lift, so replicate it.
                 * codec + advance are pure lifted C; the loop's callback_frame_boundary (0x4e041) does the
                 * modal compositing and gdv_publish_frame (popup=[0x7fec0] set) snapshots the window.
                 * PACING: 0x4e24b natively fires at a FIXED ~70 Hz GDV reference (frame rate stays constant
                 * even as MIDI bumps the PIT to 120 Hz); decoding once per SIGALRM tick (which FOLLOWS the
                 * PIT) ran ~2x fast in-game. So pace to a constant decodes/sec = 70*[0x91dbe]/0x3c00,
                 * independent of the live tick rate — same shape as the audio-cb tick_hz scaling. */
                /* Mirror native 0x4e24b's db0/dbe state machine EXACTLY (the earlier independent
                 * na_acc pacing skipped native's db0=0 caught-up BACKPRESSURE, so decode drifted out of
                 * lockstep with the ring reads and over-decoded stale-but-valid-magic records at EOS ->
                 * garbage frame -> composite callback far-called a bad pointer). Downsample the SIGALRM
                 * rate to the native ~70 Hz GDV-timer rate so db0 advances at the native cadence (correct
                 * speed even when MIDI bumps the PIT to 120 Hz). Each native tick runs every branch. */
                static double na_sub;
                double tick_hz = g_pit_div ? 1193182.0 / (double)g_pit_div : 70.0;
                /* 60 Hz, not 70: the native GDV timer fires at 60 Hz (the 0x3c=60 arg to
                 * gdv_audio_setup_voices 0x55440). MEASURED: the trap host (real timer ISR, = the
                 * original) plays the inspect-closeup at 12.00 fps = 60Hz*[dbe]/0x3c00; the old 70 gave
                 * 13.95 fps (~1.16x fast). Downsample the SIGALRM rate to that 60 Hz native cadence. */
                na_sub += 60.0 / tick_hz;            /* native 0x4e24b ticks elapsed this SIGALRM tick */
                int na_guard = 0;
                while (na_sub >= 1.0 && na_guard++ < 8) {
                    na_sub -= 1.0;
                    gdv_decode_timer_isr_noaudio();   /* 0x4e24b body (its d60==d6c backpressure keeps
                                                              * decode==reads -> no EOS over-decode; [dba]
                                                              * reaches -1 naturally to end the loop's tail) */
                    dbg_lh_fires++;
                }
            }
        } else if (!gameplay) {
            /* Non-decode interactive lift with no GDV active (gdv_fade_in/out spin on [0x91dbc],
             * gdv_read_frame_chunk on [0x91884]; or a non-GDV menu/prompt). Run the tick ISR's fade/read
             * pump — the lifted gdv_tick_timer_isr 0x4e2ed body. Not in gameplay (nothing there spins on
             * these) — its heartbeat already ran above. */
            gdv_tick_timer_isr();
        }

        /* (2) int-9 keyboard ISR stand-in — faithfully transcribe keyboard_int9_isr 0x12393 for each
         * viewer scancode (the PIC/EOI + port 0x60/0x61 handshake is the host's job, skipped). Maintain
         * BOTH the held-key bitmap g_key_state_bitmap @0x90c3c — which the movement path reads, and
         * which the old ring-only producer never updated (the "movement keys don't register" symptom) —
         * AND the input ring @0x90c1c (base; u16 head 0x7e91c, tail 0x7e91e, mask 0x7e91a) that menus read.
         * Mask table g_keybit_mask_table @0x707e9. Handles the E0/E1 extended-prefix drop and the
         * 0x2a->0x36 fake-shift fold exactly as the ISR does. */
        /* task #15 fix: FIRST reclaim any scancodes a prior NON-interactive tick shunted into the 8042
         * queue (g_kbd_q). While g_os_interactive is set, g_in_handler is too, so inject_irq(0x09)
         * bails and the real int-9 ISR never runs to drain g_kbd_q — a released movement key's BREAK
         * would sit there for the whole dispatch (the ~0.5s stick). Apply them through the identical
         * transcription. Order-preserving: g_kbd_q holds OLDER events than the shm-ring residue (it was
         * filled from the same ring on earlier ticks), so draining it first keeps make/break in order.
         * No double-apply: the real ISR is not concurrently popping g_kbd_q (its IRQ9 is bailing), and
         * once drained kbd_pending() is false so no stray IRQ9 fires after the dispatch returns. In a
         * whole-loop swap (g_os_interactive always set) g_kbd_q is never filled, so this is a no-op —
         * the validated pure-interactive path is untouched. ROTH_KBD_NO_RECLAIM=1 disables it (repro). */
        if (kbd_reclaim_on()) {
            while (kbd_pending()) {
                uint8_t sc = kbd_peek();
                kbd_pop();
                int k = iso_apply_scancode(sc);
                if (kbd_trace_on()) {
                    static int n; if (n++ < 200)
                        LOGE("[kbd] 8042 RECLAIM sc=0x%02x %s -> interactive stand-in (was stranded) frame=%u\n",
                             (unsigned)sc, k == 1 ? "BREAK" : k == 2 ? "e0/e1" : "make", g_shm->frame);
                }
            }
        }
        while (g_shm->key_tail != g_shm->key_head) {
            uint8_t sc = g_shm->key_ring[g_shm->key_tail & ROTH_KEY_MASK];
            g_shm->key_tail++;
            int k = iso_apply_scancode(sc);
            if (kbd_trace_on()) {
                static int n; if (n++ < 200)
                    LOGE("[kbd] shm-ring sc=0x%02x %s -> interactive stand-in frame=%u\n",
                         (unsigned)sc, k == 1 ? "BREAK" : k == 2 ? "e0/e1" : "make", g_shm->frame);
            }
        }
        return;   /* during an interactive lift the 8042/IRQ path is bypassed entirely */
    }

    /* Drain keyboard ring from the viewer into the 8042 queue. */
    while (g_shm->key_tail != g_shm->key_head) {
        uint8_t sc = g_shm->key_ring[g_shm->key_tail & ROTH_KEY_MASK];
        g_shm->key_tail++;
        kbd_enqueue(sc);
        if (kbd_trace_on()) {   /* task #15: this is the slow path — needs the real int-9 ISR to apply */
            static int n; if (n++ < 200)
                LOGE("[kbd] 8042 enqueue sc=0x%02x %s (non-interactive tick) frame=%u\n",
                     (unsigned)sc, (sc & 0x80) ? "BREAK" : "make", g_shm->frame);
        }
    }
}

/* The portable per-tick body, split out so both the POSIX SIGALRM handler and other OS timer
 * back-ends can run it. It does NO CPU segment-register work: on i386 signal delivery drops the host
 * TLS selectors, so alarm_handler restores fs/gs around this call, but the body itself must stay
 * segment-agnostic for back-ends whose thread selectors are already correct. */
void roth_tick_isr(void)
{
    /* imgfree-boot: force the always-interactive surrogate on so the shm_tick below drives the
     * lifted C ISR bodies directly this tick. Dead until roth_boot sets g_standalone_boot (nothing does
     * yet); idempotent belt-and-suspenders with roth_boot()'s own boot-time set. */
    if (g_standalone_boot)
        g_os_interactive = 1;

    shm_tick();   /* honors g_shm->quit -> _exit(0) internally */
}

void alarm_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
    uint16_t game_fs, game_gs;
    __asm__ volatile("mov %%fs, %0" : "=r"(game_fs));
    __asm__ volatile("mov %%gs, %0" : "=r"(game_gs));
    __asm__ volatile("mov %0, %%fs" :: "r"((uint32_t)g_host_fs));
    __asm__ volatile("mov %0, %%gs" :: "r"((uint32_t)g_host_gs));

    /* Run the portable per-tick body (frame clock, audio poll, cursor, GDV pump, keyboard drain via
     * the shm_tick surrogate) with the host TLS selectors loaded above. */
    roth_tick_isr();

    /* imgfree-boot: bypass inject_irq ENTIRELY. The frame clock, GDV accumulator pump, cursor, audio
     * poll and keyboard (iso_apply_scancode) were all produced by the shm_tick surrogate above; with obj1
     * unmapped there is no g_pm_vec handler to reflect an IRQ into, and the SOS master-timer walk 0x49eaf
     * is bypassed. The au_timer quiesce is vacuously satisfied — inject_irq is never called, so
     * g_au_timer_irq_pending never goes pending (see g_standalone_boot's contract above). sigreturn
     * restores fs/gs. Dead until g_standalone_boot is set. */
    if (g_standalone_boot)
        return;

    /* Deliver a keyboard IRQ if scancodes are waiting and none is in flight. */
    ucontext_t *uc = (ucontext_t *)ucv;
    /* audio-timer quiesce: a timer tick deferred while the SOS timer-event table was locked is
     * delivered on the first SIGALRM after the lock clears — promptly, and taking priority over a
     * queued scancode so the deferred tick is not lost. Consume the pending flag first: this is the
     * deferred tick's single delivery attempt, subject to the same inject_irq guards (IF/g_in_irq/
     * handler-installed) as any tick — matching how a normal tick is dropped under those conditions.
     * When no native ever locks, g_au_timer_irq_pending is always 0, this branch is dead, and the
     * else-if/else below is exactly the original keyboard-vs-timer decision. */
    if (g_au_timer_irq_pending && !g_au_timer_locked) {
        g_au_timer_irq_pending = 0;
        inject_irq(uc, 0x08);
    } else if (kbd_pending() && !g_in_irq)
        inject_irq(uc, 0x09);
    else
        inject_irq(uc, 0x08);

    /* sigreturn restores the interrupted fs/gs from the ucontext; no manual
     * restore (it could #GP and cascade). */
    (void)game_fs;
    (void)game_gs;
}

void irq_timer_start(void)
{
    /* PIT divisor 0x4242 -> 1193182/16962 = 70.3 Hz, as the game programs. The OS-specific timer
     * mechanism (installing alarm_handler on SIGALRM + arming the interval timer here) lives behind
     * the tick seam. */
    sys_tick_start(14222);
}

void traps_install(void)
{
    static char altstack[256 * 1024];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
    if (sigaltstack(&ss, NULL) != 0) {
        LOGE("sigaltstack failed\n");
        exit(1);
    }
    struct sigaction sa = {0};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    /* Block the calltrace control signals while this (deeply re-entrant, SA_NODEFER)
     * trap handler runs, so a SIGUSR1/2 can't set up its frame on the shared altstack
     * mid-trap — it stays pending until the game is executing natively again. */
    sigaddset(&sa.sa_mask, SIGUSR1);
    sigaddset(&sa.sa_mask, SIGUSR2);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL); /* int into bad vector can raise SIGILL */
    sigaction(SIGTRAP, &sa, NULL);
}
