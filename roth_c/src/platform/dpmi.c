/* int 31h — DPMI services, plus the int 10h/33h BIOS stubs.
 *
 * Selectors are backed by real LDT entries (modify_ldt), so game code can
 * load them into es/fs/gs and do far accesses natively — required, the
 * renderer does this constantly. Linear memory comes from mmap. Physical
 * mapping requests for the VGA window return a host framebuffer.
 */
#include "roth_host.h"
#include "shared_fb.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <execinfo.h>   /* ROTH_FB_TRACE: backtrace the render-target base-set (mirror-buffer bug) */

/* ROTH_FB_TRACE diagnostic (2026-07-09): the secondary/mirror render-target selector was seen
 * resolving to unmapped memory (segfaults in render_secondary_surface_list). This gate logs every
 * base-set of the two render-target selectors (canon 0x90c06 primary / 0x89f28 secondary) with a
 * backtrace of the engine caller. imgfree-only (g_standalone_boot); env-gated; zero cost otherwise. */
static int fb_trace_on(void)
{
    static int v = -1;
    if (v < 0) v = getenv("ROTH_FB_TRACE") ? 1 : 0;
    return v;
}

static uint32_t g_pm_vec[256];     /* CX:EDX pairs, CX implicit game CS */
static uint32_t g_exc_handler[32];
static uint32_t g_dosmem_brk = DOSMEM_LIN;

/* ---- VESA / VBE modes ---------------------------------------------------
 * Modes we advertise to the game's mode enumerator (match_vesa_video_modes
 * matches its desired resolutions against these by W/H/bpp). All are 8bpp
 * packed-pixel with a LINEAR framebuffer: the game asks for the linear variant
 * (set_vesa_video_mode passes mode|0x4000) and reads PhysBasePtr (+0x28) of the
 * mode-info block, then maps it via DPMI 0800. We back that with one host
 * buffer; the engine composes into its own back buffer and the host publishes
 * THAT (see traps.c shm_tick), so the linear FB is just a sink the present
 * writes to. Limited to modes whose frame fits the game's hi-res back buffer
 * (0x4b000 = 307200 bytes); 640x480 is the largest that fits. */
struct vbe_mode { uint16_t num, w, h; };
static const struct vbe_mode g_vbe_modes[] = {
    { 0x102, 400, 300 }, /* fits the standard framebuffer */
    { 0x103, 512, 384 }, /* needs the hi-res framebuffer (boot a hi-res mode) */
    { 0x100, 640, 400 },
    { 0x101, 640, 480 },
    /* 800x600 (480 KB) exceeds the game's largest framebuffer (0x4b000=300 KB)
     * and our publish buffer (ROTH_FB_MAX), so it is intentionally omitted. */
};
#define VBE_MODE_N ((int)(sizeof g_vbe_modes / sizeof g_vbe_modes[0]))

static const struct vbe_mode *vbe_find(uint16_t num)
{
    for (int i = 0; i < VBE_MODE_N; i++)
        if (g_vbe_modes[i].num == num)
            return &g_vbe_modes[i];
    return NULL;
}

/* Linear-framebuffer sink for VESA modes: a fixed low region mapped at startup
 * (main.c). Its linear address doubles as the "physical" base we hand the game
 * as PhysBasePtr; DPMI 0800 returns it identity, and the game reads/writes it
 * directly (so it must be a real, already-mapped page — a high mmap() address
 * from inside the trap handler was not reliably accessible). */
static uint32_t vesa_lfb(void) { return VESA_LFB_LIN; }

/* Fill a VBE ModeInfoBlock at `mib` for mode m (linear, 8bpp packed). The game
 * reads pitch from +0x12, height from +0x14, width from +0x10, bpp from +0x19,
 * model from +0x1b, and PhysBasePtr from +0x28. */
static void vbe_fill_mode_info(uint8_t *mib, const struct vbe_mode *m)
{
    memset(mib, 0, 0x100);
    *(uint16_t *)(mib + 0x00) = 0x009b;       /* attrs: supported,color,gfx,LFB */
    *(uint16_t *)(mib + 0x04) = 64;           /* win granularity (KB) */
    *(uint16_t *)(mib + 0x06) = 64;           /* win size (KB) */
    *(uint16_t *)(mib + 0x08) = 0xa000;       /* win A segment */
    *(uint16_t *)(mib + 0x10) = m->w;         /* bytes per scanline */
    *(uint16_t *)(mib + 0x12) = m->w;         /* X resolution */
    *(uint16_t *)(mib + 0x14) = m->h;         /* Y resolution */
    mib[0x18] = 1;                             /* planes */
    mib[0x19] = 8;                             /* bpp */
    mib[0x1b] = 4;                             /* memory model: packed */
    *(uint32_t *)(mib + 0x28) = vesa_lfb();    /* PhysBasePtr (linear FB) */
}

/* selector bookkeeping lives in main.c (ldt_*); here we track bases for
 * "get segment base" on our own selectors. GDT selectors report base 0. */
extern int ldt_alloc(uint32_t base, uint32_t limit_bytes);

static uint32_t sel_base_of(uint16_t sel);
static uint32_t g_known_base[8192];

static uint32_t sel_base_of(uint16_t sel)
{
    if (!(sel & 4))
        return 0; /* GDT (host flat) */
    return g_known_base[sel >> 3];
}

uint32_t dpmi_linear(uint16_t sel, uint32_t off)
{
    return sel_base_of(sel) + off;
}

uint32_t dpmi_sel_base(uint16_t sel) { return sel_base_of(sel); } /* debug */

/* Record a host-allocated selector's base in the software cache (ldt_alloc set the real descriptor
 * base already; this makes dpmi_sel_base/dpmi_linear agree, so the lifted natives' software far-ptr
 * translation resolves it). GDT selectors (bit 4 clear) report base 0 and are not cached. */
void dpmi_note_sel_base(uint16_t sel, uint32_t base)
{
    if (sel & 4)
        g_known_base[sel >> 3] = base;
}

/* ---- DPMI 0501 linear-allocation ledger --------------------------------------------------------
 * Records every int31 ax=0501 block (base, size) so consumers holding raw far pointers into DPMI
 * memory (the SOS song/patch buffers) can validate them against the REAL allocations instead of an
 * address-window guess. Motivation (task #107): haudio_midi_send guarded stream reads with the
 * empirical window 0xf3000000..0xf4000000 observed under the headless/trap process layout — but the
 * kernel picks mmap addresses, and the windowed SDL binary's extra mappings (libSDL3, audio thread,
 * plugins) shift the DPMI blocks below that window (observed 0xeeb2xxxx), so every MIDI NOTE event
 * (dispatched by raw stream pointer) was silently dropped while CC7s (staged in a canon global)
 * passed — music-specific silence. 0502 (free) is a logged no-op leak, so records are append-only. */
#define DPMI_ALLOC_MAX 256
static uint32_t g_lin_alloc_base[DPMI_ALLOC_MAX], g_lin_alloc_size[DPMI_ALLOC_MAX];
static unsigned g_lin_alloc_n;

static void dpmi_lin_alloc_record(uint32_t base, uint32_t size)
{
    if (g_lin_alloc_n < DPMI_ALLOC_MAX) {
        g_lin_alloc_base[g_lin_alloc_n] = base;
        g_lin_alloc_size[g_lin_alloc_n] = size;
        g_lin_alloc_n++;
    }
}

int dpmi_lin_alloc_contains(uint32_t lin, uint32_t len)
{
    for (unsigned i = 0; i < g_lin_alloc_n; i++)
        if (lin >= g_lin_alloc_base[i] && lin + len <= g_lin_alloc_base[i] + g_lin_alloc_size[i])
            return 1;
    return 0;
}

/* Host-backed DPMI PM interrupt-vector accessors for the image-free timer natives (configure/stop,
 * os_audio.c), reproducing the int31 ax=0204(get)/0205(set) arm of the SOS vector
 * install/teardown (0x54bc7/0x54ce1) taken when [0x755b8]!=0. Same g_pm_vec[] the case 0x0204/0x0205
 * handlers read/write. NB in-game that arm is UNREACHED: sos_configure_timer_rate's single caller
 * passes flags=0 -> [0x755b8]==0 -> the DOS int21 25/35 arm (which touches g_pm_vec_int21, the array
 * inject_irq actually delivers through). These keep the transcription fully host-backed regardless. */
uint32_t dpmi_get_pm_vec(uint8_t v) { return g_pm_vec[v]; }
void     dpmi_set_pm_vec(uint8_t v, uint32_t va) { g_pm_vec[v] = va; }

void dpmi_int31(cpu_t *c)
{
    uint16_t ax = (uint16_t)(R_EAX(c) & 0xffff);
    set_cf(c, 0);

    switch (ax) {
    case 0x0000: { /* allocate CX descriptors */
        int n = (int)(R_ECX(c) & 0xffff);
        int sel = ldt_alloc(0, 0xfffff);
        for (int i = 1; i < n; i++)
            ldt_alloc(0, 0xfffff); /* consecutive entries by construction */
        LOGT("dpmi 0000: alloc %d descriptors -> 0x%x\n", n, sel);
        if (sel < 0) {
            set_cf(c, 1);
            break;
        }
        R_EAX(c) = (uint32_t)sel;
        break;
    }
    case 0x0001: /* free descriptor BX */
        LOGT("dpmi 0001: free sel 0x%x\n", R_EBX(c) & 0xffff);
        ldt_free((uint16_t)R_EBX(c));
        g_known_base[(R_EBX(c) & 0xffff) >> 3] = 0;
        break;
    case 0x0003: /* selector increment */
        R_EAX(c) = 8;
        break;
    case 0x0006: { /* get segment base of BX -> CX:DX */
        uint32_t base = sel_base_of((uint16_t)R_EBX(c));
        R_ECX(c) = (R_ECX(c) & ~0xffffu) | (base >> 16);
        R_EDX(c) = (R_EDX(c) & ~0xffffu) | (base & 0xffff);
        LOGT("dpmi 0006: base of 0x%x -> 0x%x\n", R_EBX(c) & 0xffff, base);
        break;
    }
    case 0x0007: { /* set segment base CX:DX */
        uint32_t base = ((R_ECX(c) & 0xffff) << 16) | (R_EDX(c) & 0xffff);
        uint16_t sel = (uint16_t)R_EBX(c);
        g_known_base[sel >> 3] = base;
        if (ldt_set_base(sel, base) != 0)
            set_cf(c, 1);
        LOGT("dpmi 0007: sel 0x%x base=0x%x\n", sel, base);
        if (g_standalone_boot && fb_trace_on()) {
            /* the two render-target selectors are stored in the obj3 arena at canon 0x90c06
             * (primary) / 0x89f28 (secondary); runtime = canon + OBJ_DELTA. Log + backtrace only
             * when THIS base-set is one of them, so we see the exact engine caller and the resulting
             * primary/secondary base pair (the mirror buffer bug = secondary base unmapped). */
            uint16_t rp = *(volatile uint16_t *)(uintptr_t)(0x90c06u + OBJ_DELTA);
            uint16_t rs = *(volatile uint16_t *)(uintptr_t)(0x89f28u + OBJ_DELTA);
            const char *which = (sel == rp) ? "PRIMARY" : (sel == rs) ? "SECONDARY" : NULL;
            if (which) {
                LOGE("[fbtrace] set-base RENDER-TARGET %s sel 0x%x -> base 0x%x  "
                     "[now: primary sel 0x%x base 0x%x | secondary sel 0x%x base 0x%x]\n",
                     which, sel, base, rp, g_known_base[rp >> 3], rs, g_known_base[rs >> 3]);
                void *bt[32];
                int n = backtrace(bt, 32);
                backtrace_symbols_fd(bt, n, 2);
            }
        }
        break;
    }
    case 0x0008: { /* set segment limit CX:DX */
        uint32_t limit = ((R_ECX(c) & 0xffff) << 16) | (R_EDX(c) & 0xffff);
        uint16_t sel = (uint16_t)R_EBX(c);
        if (ldt_set_limit(sel, limit) != 0)
            set_cf(c, 1);
        LOGT("dpmi 0008: sel 0x%x limit=0x%x\n", sel, limit);
        break;
    }
    case 0x0009: /* set access rights: data/code bits — accept */
        LOGT("dpmi 0009: sel 0x%x rights=0x%x\n", R_EBX(c) & 0xffff,
             R_ECX(c) & 0xffff);
        break;
    case 0x000a: { /* create alias of BX */
        uint16_t src = (uint16_t)R_EBX(c);
        int sel = ldt_alloc(sel_base_of(src), 0xfffff);
        if (sel < 0) {
            set_cf(c, 1);
            break;
        }
        g_known_base[sel >> 3] = sel_base_of(src);
        R_EAX(c) = (uint32_t)sel;
        break;
    }
    case 0x0100: { /* allocate DOS memory: BX paragraphs */
        uint32_t bytes = (R_EBX(c) & 0xffff) * 16u;
        uint32_t lin = (g_dosmem_brk + 15) & ~15u;
        if (lin + bytes > DOSMEM_LIN + DOSMEM_SIZE) {
            /* Too big for the <1 MB conventional pool. The game uses these via
             * their real-mode segment, so a mmap fallback (no valid RM segment)
             * crashes — let it fail cleanly; the game has a graceful fallback. */
            LOGT("dpmi 0100: DOS-mem pool exhausted (%u bytes) — failing\n", bytes);
            set_cf(c, 1);
            R_EAX(c) = 8;
            break;
        }
        g_dosmem_brk = lin + bytes;
        int sel = ldt_alloc(lin, bytes ? bytes - 1 : 0);
        g_known_base[sel >> 3] = lin;
        R_EAX(c) = lin >> 4; /* real-mode segment */
        R_EDX(c) = (uint32_t)sel;
        LOGT("dpmi 0100: %u bytes at 0x%x sel 0x%x\n", bytes, lin, sel);
        break;
    }
    case 0x0101: /* free DOS memory */
        break;
    case 0x0200: /* get real-mode vector -> CX:DX (fake) */
        R_ECX(c) = (R_ECX(c) & ~0xffffu);
        R_EDX(c) = (R_EDX(c) & ~0xffffu);
        break;
    case 0x0201: /* set real-mode vector */
        LOGT("dpmi 0201: rm vector 0x%02x\n", R_EBX(c) & 0xff);
        break;
    case 0x0202: /* get exception handler */
        R_ECX(c) = (R_ECX(c) & ~0xffffu) | 0x23; /* host code sel-ish */
        R_EDX(c) = g_exc_handler[R_EBX(c) & 0x1f];
        break;
    case 0x0203: /* set exception handler */
        g_exc_handler[R_EBX(c) & 0x1f] = R_EDX(c);
        LOGT("dpmi 0203: exception 0x%02x handler=0x%x\n", R_EBX(c) & 0x1f,
             R_EDX(c));
        break;
    case 0x0204: /* get PM interrupt vector -> CX:EDX */
        R_ECX(c) = (R_ECX(c) & ~0xffffu) | 0x23;
        R_EDX(c) = g_pm_vec[R_EBX(c) & 0xff];
        break;
    case 0x0205: /* set PM interrupt vector */
        g_pm_vec[R_EBX(c) & 0xff] = R_EDX(c);
        LOGE("dpmi 0205: PM vector 0x%02lx = 0x%x%s\n",
             (unsigned long)(R_EBX(c) & 0xff), R_EDX(c),
             (R_EBX(c) & 0xff) == 0x08 ? "  <-- TIMER"
             : (R_EBX(c) & 0xff) == 0x09 ? "  <-- KEYBOARD" : "");
        break;
    case 0x0300: { /* simulate real-mode interrupt; RM register struct ES:EDI */
        uint8_t *rm = (uint8_t *)dpmi_linear((uint16_t)R_ES(c), R_EDI(c));
        uint32_t rm_eax = *(uint32_t *)(rm + 0x1c);
        uint16_t *rm_flags = (uint16_t *)(rm + 0x20);
        uint8_t vec = (uint8_t)(R_EBX(c) & 0xff);
        if (vec == 0x10 && (rm_eax & 0xffff) == 0x4f00 && g_vesa) {
            /* VBE get controller info -> VbeInfoBlock at rm ES:DI */
            uint32_t off = *(uint16_t *)(rm + 0x00);   /* rm EDI low = DI */
            uint16_t seg = *(uint16_t *)(rm + 0x22);   /* rm ES */
            uint8_t *vbe = (uint8_t *)(uintptr_t)((seg << 4) + off);
            memcpy(vbe, "VESA", 4);
            *(uint16_t *)(vbe + 0x04) = 0x0200;        /* VBE 2.0 */
            *(uint16_t *)(vbe + 0x12) = 16;            /* 1 MB / 64K */
            uint16_t *modes = (uint16_t *)(vbe + 0x100);
            for (int i = 0; i < VBE_MODE_N; i++)
                modes[i] = g_vbe_modes[i].num;
            modes[VBE_MODE_N] = 0xffff;                /* mode-list terminator */
            *(uint16_t *)(vbe + 0x0e) = (uint16_t)(off + 0x100); /* VideoModePtr off */
            *(uint16_t *)(vbe + 0x10) = seg;           /* VideoModePtr seg */
            *(uint32_t *)(rm + 0x1c) = 0x004f;         /* AX = success */
            *rm_flags &= ~1u;
            LOGT("dpmi 0300: VBE 4F00 -> VESA present (%d modes)\n", VBE_MODE_N);
        } else if (vec == 0x10 && (rm_eax & 0xffff) == 0x4f01 && g_vesa) {
            /* VBE get mode info -> ModeInfoBlock at rm ES:DI; mode in rm CX */
            uint16_t mode = *(uint16_t *)(rm + 0x18) & 0x1ff; /* rm ECX low */
            uint32_t off = *(uint16_t *)(rm + 0x00);
            uint16_t seg = *(uint16_t *)(rm + 0x22);
            uint8_t *mib = (uint8_t *)(uintptr_t)((seg << 4) + off);
            const struct vbe_mode *m = vbe_find(mode);
            if (m) {
                vbe_fill_mode_info(mib, m);
                *(uint32_t *)(rm + 0x1c) = 0x004f;
                *rm_flags &= ~1u;
            } else {
                *(uint32_t *)(rm + 0x1c) = 0x014f;     /* unsupported mode */
                *rm_flags &= ~1u;
            }
            LOGT("dpmi 0300: VBE 4F01 mode 0x%x -> %s\n", mode, m ? "ok" : "unsupported");
        } else if (vec == 0x21 && AH_OF(rm_eax) == 0x63) {
            /* get DBCS lead-byte table -> DS:SI; empty table = US DOS */
            memset((void *)DOSMEM_LIN, 0, 4);
            *(uint16_t *)(rm + 0x24) = DOSMEM_LIN >> 4; /* RM DS */
            *(uint32_t *)(rm + 0x04) = DOSMEM_LIN & 0xf; /* RM ESI */
            *(uint32_t *)(rm + 0x1c) = rm_eax & ~0xffu;  /* AL=0 ok */
            *rm_flags &= ~1u; /* CF clear */
            LOGT("dpmi 0300: rm int21 AH=63 (DBCS table) -> empty\n");
        } else {
            LOGE("dpmi 0300: rm int 0x%02x ax=%04x — failing in-struct\n", vec,
                 rm_eax & 0xffff);
            *rm_flags |= 1u; /* CF set inside the simulated frame */
        }
        break;
    }
    case 0x0400: /* DPMI version */
        R_EAX(c) = 0x005a; /* 0.90 */
        R_EBX(c) = 0x0001; /* 32-bit host */
        R_ECX(c) = 4;      /* 486 */
        R_EDX(c) = 0;
        break;
    case 0x0500: { /* get free memory info into ES:EDI */
        uint8_t *buf = (uint8_t *)dpmi_linear((uint16_t)R_ES(c), R_EDI(c));
        memset(buf, 0xff, 0x30);
        *(uint32_t *)buf = 64u << 20; /* largest free block: 64 MB */
        break;
    }
    case 0x0501: { /* allocate linear memory BX:CX -> BX:CX addr, SI:DI handle */
        uint32_t size = ((R_EBX(c) & 0xffff) << 16) | (R_ECX(c) & 0xffff);
        void *p = mmap(NULL, (size + 0xfff) & ~0xfffu, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        LOGT("dpmi 0501: alloc 0x%x -> %p\n", size, p);
        if (p == MAP_FAILED) {
            set_cf(c, 1);
            break;
        }
        dpmi_lin_alloc_record((uint32_t)(uintptr_t)p, (size + 0xfff) & ~0xfffu);
        uint32_t lin = (uint32_t)p;
        R_EBX(c) = (R_EBX(c) & ~0xffffu) | (lin >> 16);
        R_ECX(c) = (R_ECX(c) & ~0xffffu) | (lin & 0xffff);
        R_ESI(c) = (R_ESI(c) & ~0xffffu) | (lin >> 16);
        R_EDI(c) = (R_EDI(c) & ~0xffffu) | (lin & 0xffff);
        break;
    }
    case 0x0502: /* free linear memory (handle in SI:DI) — leak for now */
        LOGT("dpmi 0502: free handle %04x:%04x\n", R_ESI(c) & 0xffff,
             R_EDI(c) & 0xffff);
        break;
    case 0x0600: case 0x0601: /* lock/unlock linear region */
        break;
    case 0x0800: { /* map physical CX:BX size SI:DI -> identity (VGA is real
                    * at 0xA0000 in our address space) */
        uint32_t phys = ((R_EBX(c) & 0xffff) << 16) | (R_ECX(c) & 0xffff);
        uint32_t size = ((R_ESI(c) & 0xffff) << 16) | (R_EDI(c) & 0xffff);
        LOGE("dpmi 0800: map phys 0x%x size 0x%x -> identity\n", phys, size);
        R_EBX(c) = (R_EBX(c) & ~0xffffu) | (phys >> 16);
        R_ECX(c) = (R_ECX(c) & ~0xffffu) | (phys & 0xffff);
        break;
    }
    case 0x0801: /* free physical mapping */
        break;
    default:
        LOGE("UNIMPLEMENTED dpmi ax=%04x (ebx=%08x ecx=%08x edx=%08x) eip 0x%x\n",
             ax, R_EBX(c), R_ECX(c), R_EDX(c), R_EIP(c));
        exit(5);
    }
}

/* --------------------------------------------------------- int 10h/33h -- */

void video_int10(cpu_t *c)
{
    uint16_t ax = (uint16_t)(R_EAX(c) & 0xffff);
    uint8_t ah = AH_OF(ax);
    if (ah == 0x0f) { /* get current mode */
        R_EAX(c) = (R_EAX(c) & ~0xffffu) | 0x5003; /* 80 cols would be BH */
        R_EBX(c) &= ~0xff00u;                      /* page 0 */
        return;
    }
    if (ah == 0x00) { /* set mode: standard VGA modes (13h = 320x200) */
        LOGE("int10: set video mode 0x%02x\n", AL_OF(ax));
        vesa_set_mode(0); /* leave hi-res */
        return;
    }
    if (ax == 0x1a00) { /* get display combination: VGA with analog color */
        R_EAX(c) = (R_EAX(c) & ~0xffffu) | 0x1a1a;
        R_EBX(c) = (R_EBX(c) & ~0xffffu) | 0x0008;
        return;
    }
    if (ah == 0x4f) { /* VESA BIOS extensions */
        if (!g_vesa) { /* hi-res disabled: report no VBE (faithful 320x200) */
            R_EAX(c) = (R_EAX(c) & ~0xffffu) | 0x014f;
            return;
        }
        uint8_t fn = AL_OF(ax);
        switch (fn) {
        case 0x02: { /* set mode (BX): one of our advertised VBE modes */
            uint16_t mode = (uint16_t)(R_EBX(c) & 0x1ff);
            const struct vbe_mode *m = vbe_find(mode);
            /* Our VESA modes display via the engine's linear back buffer
             * (traps.c shm_tick), not the banked g_hires window — clear any
             * banked state so it doesn't shadow the back-buffer publish. */
            vesa_set_mode(0);
            if (m)
                LOGE("int10: VESA set mode 0x%x (%ux%u, linear back-buffer)\n",
                     mode, m->w, m->h);
            else
                LOGE("int10: VESA set mode 0x%x (unadvertised)\n", mode);
            break;
        }
        case 0x05: /* window control: BH=0 set / 1 get, BL=window, DX=bank */
            if (((R_EBX(c) >> 8) & 0xff) == 0)
                vesa_set_bank((int)(R_EDX(c) & 0xffff));
            else
                R_EDX(c) = (R_EDX(c) & ~0xffffu) | (vesa_get_bank() & 0xffff);
            break;
        case 0x07: /* set/get display start = the VESA page flip; publish the
                    * now-complete back buffer in sync with it (see traps.c).
                    * CX=first pixel in scanline (x), DX=first scanline (y); record
                    * the displayed LFB page for the inspect-popup publish path. */
            vesa_set_display_start(R_EDX(c) & 0xffff, R_ECX(c) & 0xffff);
            video_publish_composed();
            break;
        case 0x03: /* get current mode */
            R_EBX(c) = (R_EBX(c) & ~0xffffu) | (g_hires ? 0x101 : 0x13);
            break;
        default:
            LOGT("int10: VESA fn 0x%02x (accepted no-op)\n", fn);
            break;
        }
        R_EAX(c) = (R_EAX(c) & ~0xffffu) | 0x004f; /* success */
        return;
    }
    LOGE("int10: ax=%04x (stub, no-op)\n", ax);
}

void mouse_int33(cpu_t *c)
{
    uint16_t ax = (uint16_t)(R_EAX(c) & 0xffff);
    int mx = g_shm ? g_shm->mouse_x : 0;
    int my = g_shm ? g_shm->mouse_y : 0;
    uint32_t btn = g_shm ? g_shm->mouse_buttons : 0;

    switch (ax) {
    case 0x0000: /* reset + presence: report present, 2 buttons */
        R_EAX(c) = (R_EAX(c) & ~0xffffu) | 0xffff;
        R_EBX(c) = (R_EBX(c) & ~0xffffu) | 2;
        break;
    case 0x0003: /* get position + button status */
        R_ECX(c) = (R_ECX(c) & ~0xffffu) | (uint32_t)(mx & 0xffff);
        R_EDX(c) = (R_EDX(c) & ~0xffffu) | (uint32_t)(my & 0xffff);
        R_EBX(c) = (R_EBX(c) & ~0xffffu) | btn;
        break;
    case 0x000b: { /* read relative motion counters (mickeys) since last call */
        int dx = 0, dy = 0;
        if (g_shm) {
            dx = g_shm->mouse_dx;
            dy = g_shm->mouse_dy;
            g_shm->mouse_dx -= dx; /* consume only what we read (viewer may add) */
            g_shm->mouse_dy -= dy;
        }
        R_ECX(c) = (R_ECX(c) & ~0xffffu) | (uint32_t)(dx & 0xffff);
        R_EDX(c) = (R_EDX(c) & ~0xffffu) | (uint32_t)(dy & 0xffff);
        break;
    }
    case 0x0004: /* set position — accept silently */
    case 0x0001: /* show cursor */
    case 0x0002: /* hide cursor */
    case 0x0007: /* set horizontal range */
    case 0x0008: /* set vertical range */
    case 0x000c: /* set event handler */
    case 0x000f: /* set mickey/pixel ratio */
        break;
    default:
        LOGT("int33 ax=%04x (stub)\n", ax);
        break;
    }
}
