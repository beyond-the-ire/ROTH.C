/* lift_dos_runtime.c — the ROTH dos_runtime subsystem lifted to verified C.
 * Per docs/operating/recomp.md §4.6: every subsystem gets its own TU.
 *
 * dos_runtime = the engine-side DOS/DPMI/OS glue that isn't bucketed into the host CRT/DPMI
 * classes — errno mapping, the timer (INT 8) hook, exception/critical-error handlers, DOS memory
 * blocks, brk grow, DBCS, and the spawn/exec family.
 * lift-lens: docs/reference/lift/dos_runtime.md.
 *
 * The subsystem is bridge-heavy (most of it is the OS seam: DPMI int 0x31, ISR install/restore,
 * DOS EXEC shell-out). THIS file lifts only the cluster of PURE, oracle-verifiable leaves — the
 * errno setters, the dword memset, the DOS command-tail string builder, and the DOS-error→errno
 * mapper. The DPMI/ISR/spawn clusters are DEFERRED to a dedicated debugging session (they need
 * g_os_soft_int routing + in-game validation, or their native-port reachability confirmed first).
 *
 * ABI throughout is derived from the DISASM (tools/roth_disasm.py func <va>), not the corpus
 * pseudocode. These leaves use Watcom register args (EAX/EDX/EBX/ECX) — confirmed per function.
 */
#include "common.h"
#include "engine.h"
#include "os_api.h"
#include <string.h>

/* ---- set_errno (0x55ba1): *errno = value; returns &errno ------------------------------------
 *   push edx; mov edx,eax; call get_errno_ptr (eax=&errno); mov [eax],edx; pop edx; ret
 * EAX = errno value in; returns EAX = &errno (the getter leaves it in eax through the ret). A4:
 * &errno is a runtime CRT address (get_errno_ptr returns 0x97d44+OBJ_DELTA) — write it raw. */
uint32_t set_errno(uint32_t value)
{
    uint32_t p = get_errno_ptr();
    *(volatile uint32_t *)(uintptr_t)p = value;
    return p;
}

/* ---- set_doserrno (0x55bc4): *_doserrno = value; returns &_doserrno --------------------------
 * Sibling of set_errno through get_doserrno_ptr (0x97d48). EAX in, EAX=&_doserrno out. */
uint32_t set_doserrno(uint32_t value)
{
    uint32_t p = get_doserrno_ptr();
    *(volatile uint32_t *)(uintptr_t)p = value;
    return p;
}

/* ---- memset_fill_dwords (0x55277): fill `count` dwords at `dst` with `val` -------------------
 * EAX=dst, EDX=val, ECX=count(dwords). The original is a Watcom unrolled dword-store with a
 * 32-byte alignment prefill, an 8-dwords/iteration body, and a `cmp byte[eax+0x20],dl` cache
 * PREFETCH (a discarded read). None of that is observable: the write-set is exactly dst[0..count-1]
 * := val, and EAX walks the buffer forward by 4/dword so the final EAX = dst + count*4 in every
 * path (traced: count=0/1/8/12/16 all end at dst+count*4). So a plain faithful loop is byte-exact
 * over the write-set AND the return register. Verified by the oracle (test_dos_runtime.c). */
uint32_t memset_fill_dwords(uint32_t dst, uint32_t val, uint32_t count)
{
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)dst;
    for (uint32_t i = 0; i < count; i++)
        p[i] = val;
    return dst + count * 4u;   /* == the address the original leaves in EAX */
}

/* ---- build_dos_command_tail (0x55a33): assemble a space-joined arg tail from argv[1..] --------
 * EDX=argv (char** — argv[0] is the program name and is SKIPPED), EBX=dest buffer, ECX=mode.
 *   mode != 0 : C-string mode — args joined by ' ', NUL-terminated at the end.
 *   mode == 0 : DOS PSP command-tail mode — byte[0] reserved for a length, args joined by ' ',
 *               a CR (0x0D) appended, and byte[0] = (#chars written) via an 8-bit ptr-delta.
 * The per-arg copy is string_copy_bytewise (0x558ec): copies incl. the NUL and returns a pointer
 * to that NUL. It is inlined here so we control the exact end pointer (the lifted helper is void).
 * Verified via the full obj1+obj3 write-set (the dest buffer is the contract). */
uint32_t build_dos_command_tail(uint32_t argv, uint32_t dest, uint32_t mode)
{
    uint32_t *av        = (uint32_t *)(uintptr_t)argv;    /* esi */
    uint8_t  *bufstart  = (uint8_t  *)(uintptr_t)dest;    /* [ebp-4] */
    uint8_t  *b         = bufstart;                       /* ebx */
    uint8_t  *end       = bufstart;                       /* tracks string_copy return (eax) */

    if (mode == 0)
        b++;                                             /* reserve byte[0] for the length */

    if (av[0] == 0) goto done;                           /* argv[0] (program name) */
    av++;                                                /* -> argv[1] */
    if (av[0] == 0) goto done;                           /* no first real arg */

    for (;;) {
        uint8_t       *d = b;                            /* eax = ebx */
        const uint8_t *s = (const uint8_t *)(uintptr_t)av[0];  /* edx = [esi] */
        av++;                                            /* add esi,4 */
        while ((*d = *s) != 0) { d++; s++; }             /* string_copy_bytewise; d -> the NUL */
        end = d;                                         /* eax */
        b   = d;                                         /* mov ebx,eax */
        if (av[0] == 0) goto done;                       /* next arg NULL -> stop */
        b = d + 1;                                       /* lea ebx,[eax+1] */
        *d = 0x20;                                       /* overwrite NUL with a space */
    }

done:
    if (mode != 0) {
        *b = 0;                                          /* NUL-terminate */
        return (uint32_t)(uintptr_t)end;
    }
    /* DOS PSP tail: length byte at [0] = (dest_ptr - bufstart) - 1, via an 8-bit delta (the
     * original does `mov al,bl; sub al,byte[ebp-4]; dec al`), then a CR at the current position. */
    uint8_t al = (uint8_t)((uint8_t)(uintptr_t)b - (uint8_t)(uintptr_t)bufstart);
    *b = 0x0d;
    al = (uint8_t)(al - 1);
    *bufstart = al;
    return ((uint32_t)(uintptr_t)end & 0xffffff00u) | al;   /* eax = end with AL replaced */
}

/* ==================== cluster C — the DOS/DPMI memory-block allocators ====================
 * These carried inline `int 0x31`s (the reason they were deferred). The OS touch now goes
 * through the C2 call-API (os_api.h): the host binding reuses the trap-side DPMI services as
 * calls; the oracle binding is the canned double whose SIGTRAP servicer also feeds the patched
 * ORIGINAL — so these are verified by a TRUE differential over identical canned DPMI.
 *
 * The CRT block-list model (head g_os_block_list 0x8a270; 0x14-byte header before the data):
 *   [hdr+0] next  [hdr+4] prev  [hdr+8] DPMI handle / DOS selector  [hdr+0xc] requested size
 *   [hdr+0x10] flags (1 = DPMI linear block, 2 = DOS conventional block)
 * alloc returns hdr+0x14; free takes the data ptr back. */

/* shared link tail (0x35ab9..0x35ae6, reached from BOTH allocators — Watcom shared code):
 * requested size + first-data-dword, push onto the doubly-linked list head, return the data. */
static uint32_t dr_link_block(uint32_t hdr, uint32_t req_size)
{
    *(volatile uint32_t *)(uintptr_t)(hdr + 0xc)  = req_size;
    *(volatile uint32_t *)(uintptr_t)(hdr + 0x14) = 0;
    uint32_t head = (uint32_t)G32(VA_g_block_list_head);
    if (head != 0)
        *(volatile uint32_t *)(uintptr_t)(head + 4) = hdr;    /* oldhead->prev = hdr */
    *(volatile uint32_t *)(uintptr_t)(hdr + 4) = 0;           /* hdr->prev = 0 */
    *(volatile uint32_t *)(uintptr_t)hdr = (uint32_t)G32(VA_g_block_list_head);  /* hdr->next = oldhead */
    G32(VA_g_block_list_head) = (int32_t)hdr;
    return hdr + 0x14;
}

/* ---- alloc_dpmi_block (0x35a74) — the CRT DPMI linear-block allocator -------------------------
 * CRT-klass (NOT in the gate) but lifted so its engine callers go call-closed. EAX = size.
 * Rounds size+0x14 up to 4K (`and ax,0xf000` — low-word mask), int31 0501, aligns the base to 4
 * (`and al,0xfc`), stamps handle/flags-1, links, returns hdr+0x14; 0 on CF or a NULL base. */
uint32_t alloc_dpmi_block(uint32_t size)
{
    uint32_t rounded = size + 0x14 + 0xfff;
    rounded = (rounded & 0xffff0000u) | (rounded & 0xf000u); /* and ax,0xf000 */
    uint32_t base, handle;
    if (os_dpmi_alloc_linear(rounded, &base, &handle))
        return 0;                                            /* jb 0x35aee */
    if (base == 0)
        return 0;                                            /* je 0x35ae7 (EAX = base = 0) */
    uint32_t hdr = (base + 3) & ~3u;                         /* add eax,3; and al,0xfc */
    *(volatile uint32_t *)(uintptr_t)(hdr + 8)    = handle;
    *(volatile uint32_t *)(uintptr_t)(hdr + 0x10) = 1;       /* DPMI linear block */
    return dr_link_block(hdr, size);
}

/* ---- alloc_dos_block (0x35a12): allocate DOS conventional memory into the block list ---------
 * EAX = size. paragraphs = (size+0x23)>>4, int31 0100; hdr = seg<<4 (a linear address in low
 * memory), stamps selector/flags-2, then falls into the SHARED link tail (`jmp 0x35ab9` — the
 * multi-entry pattern). Returns hdr+0x14; 0 on CF or segment 0. */
uint32_t alloc_dos_block(uint32_t size)
{
    uint32_t seg = 0, sel = 0;
    if (os_dpmi_alloc_dos((size + 0x23) >> 4, &seg, &sel))
        return 0;                                            /* jb 0x35a4b: sub eax,eax */
    uint32_t hdr = (seg & 0xffffu) << 4;
    if (hdr == 0)
        return 0;                                            /* je 0x35a4d (EAX = 0) */
    *(volatile uint32_t *)(uintptr_t)(hdr + 8)    = sel & 0xffffu;
    *(volatile uint32_t *)(uintptr_t)(hdr + 0x10) = 2;       /* DOS conventional block */
    return dr_link_block(hdr, size);
}

/* ---- free_os_block (0x35b0a): unlink + free a block by its data pointer ----------------------
 * EAX = data ptr (or 0 = no-op). Unlinks hdr = ptr-0x14 from the list (head 0x8a270), then frees
 * the OS side: flags bit1 -> DOS free by selector (int31 0101, `mov edx,[hdr+8]`); else DPMI
 * linear free by handle (int31 0502, SI:DI = the stored dword). Returns 0 (sub eax,eax). */
uint32_t free_os_block(uint32_t ptr)
{
    if (ptr != 0) {
        uint32_t hdr  = ptr - 0x14;
        uint32_t prev = *(volatile uint32_t *)(uintptr_t)(hdr + 4);
        uint32_t next = *(volatile uint32_t *)(uintptr_t)hdr;
        if (next != 0)
            *(volatile uint32_t *)(uintptr_t)(next + 4) = prev;
        if (prev != 0)
            *(volatile uint32_t *)(uintptr_t)prev = next;
        else
            G32(VA_g_block_list_head) = (int32_t)next;
        if (*(volatile uint8_t *)(uintptr_t)(hdr + 0x10) & 2)
            os_dpmi_free_dos(*(volatile uint32_t *)(uintptr_t)(hdr + 8) & 0xffffu);
        else
            os_dpmi_free_linear(*(volatile uint32_t *)(uintptr_t)(hdr + 8));
    }
    return 0;
}

/* ---- free_os_block_guarded (0x35af2): free via the resource pool when it exists --------------
 * EAX = ptr. If g_resource_pool_for_os_blocks (0x8a274) != 0: pool_free_chunk(pool, ptr) —
 * returns its EAX; else FALLS INTO free_os_block (the je targets 0x35b0a — shared code). */
uint32_t free_os_block_guarded(uint32_t ptr)
{
    uint32_t pool = (uint32_t)G32(VA_g_block_list_head + 0x4);
    if (pool != 0)
        return pool_free_chunk((uint32_t *)(uintptr_t)pool, (uint8_t *)(uintptr_t)ptr);
    return free_os_block(ptr);
}

/* ---- dpmi_alloc_dos_memory (0x40a34) — CRT helper (NOT in the gate) --------------------------
 * EAX = bytes -> paragraphs (+0xf)>>4, int31 0100. Success: EAX = segment (zero-extended), DX =
 * selector (a live non-EAX output — gotcha A1). Fail: EAX = 0 + STC, DX untouched. *sel_io
 * carries the caller's DX in and the selector out (untouched on fail, as the original). */
uint32_t dpmi_alloc_dos_memory(uint32_t bytes, uint32_t *sel_io)
{
    uint32_t seg = 0, sel = *sel_io;
    if (os_dpmi_alloc_dos((bytes + 0xf) >> 4, &seg, &sel)) {
        *sel_io = sel;                                       /* untouched by the mock/host on fail */
        return 0;                                            /* sub eax,eax; stc */
    }
    *sel_io = sel;
    return seg & 0xffffu;
}

/* ---- ensure_dos_transfer_buffer (0x40b08): lazily allocate the 4K DOS transfer buffer --------
 * If g_dos_transfer_lin (0x8c1f8) == 0: dpmi_alloc_dos_memory(0x1000) -> selector word to
 * 0x8c1fc, segment<<4 (linear) to 0x8c1f8. On the alloc's fail path the original stores its own
 * (caller-inherited) DX to 0x8c1fc and 0 to 0x8c1f8 — edx_in reproduces that artifact. */
void ensure_dos_transfer_buffer(uint32_t edx_in)
{
    if ((uint32_t)G32(VA_g_dos_transfer_buffer_linear) != 0)
        return;
    uint32_t sel = edx_in;
    uint32_t seg = dpmi_alloc_dos_memory(0x1000, &sel);
    G16(VA_g_dos_transfer_buffer_selector) = (uint16_t)sel;
    G32(VA_g_dos_transfer_buffer_linear) = (int32_t)((seg & 0xffffu) << 4);
}

/* ==================== cluster D — the timer-tick ISR chain (in-game tier) ====================
 * ISR-body pattern from the input subsystem (keyboard_int9_isr): the hardware dance —
 * pushal/segment reloads, PIT reprogramming (out 0x43/0x40), EOI (out 0x20), the BIOS-vector
 * chain, iretd — is the HOST-REPLACED seam; the C body owns everything engine-visible. Verified
 * by the in-game tier (ISRs fire under the host timer — D1 non-determinism); never int3-swapped
 * (vector targets); the live tier is the host timer surrogate calling the bodies directly. */

/* ---- vsync_timer_tick (0x122e3): the per-tick engine body -----------------------------------
 * inc word tick 0x90bcc; if byte[0x7674a]&1: ES:=DS (flat — implicit here) then
 * update_software_cursor 0x116b6 + player_movement_tick 0x12520 (both lifted -> direct C);
 * then the installed frame-tick hook [0x7e8d4] (a RUNTIME fn ptr the game registers) is called
 * under pushf/sti/popf — bridged via call_orig (whatever code the game registered). */
void vsync_timer_tick(void)
{
    G16(VA_g_frame_tick_counter) = (uint16_t)((uint16_t)G16(VA_g_frame_tick_counter) + 1);
    if (G8(VA_g_player_movement_enabled) & 1) {
        update_software_cursor();                    /* 0x116b6 */
        player_movement_tick();                      /* 0x12520 */
    }
    uint32_t hook = (uint32_t)G32(VA_g_frame_tick_callback);                 /* the installed frame-tick hook */
    if (hook != 0) {
        regs_t io; memset(&io, 0, sizeof io);
        io.va = hook;                                       /* already a runtime address */
#ifndef ROTH_STANDALONE
        call_orig(&io);
#else
        roth_unreachable(hook - OBJ_DELTA);                /* installed frame-tick hook (code-ptr) — in-game timer tier */
#endif
    }
}

/* ---- wait_one_timer_tick (0x2e91a): busy-wait until the tick passes tick+1 -------------------
 * dx = tick+1; loop: ax = dx - tick; jns loop. Exits when the tick advances past the target
 * (16-bit wrap-safe). INTERACTIVE: blocks until the ISR (or the host surrogate under
 * g_os_interactive) advances 0x90bcc — in-game tier only. */
void wait_one_timer_tick(void)
{
    uint16_t target = (uint16_t)((uint16_t)G16(VA_g_frame_tick_counter) + 1);
    for (;;) {
        int16_t d = (int16_t)(uint16_t)(target - (uint16_t)G16(VA_g_frame_tick_counter));
        if (d < 0) break;                                   /* jns keeps looping */
    }
}

/* ---- game_heartbeat_timer_isr (0x12336): the INT 8 (IRQ0) ISR body ---------------------------
 * Hardware seam (host-replaced): pushal + ds/es from cs:[0x2ef54], the PIT latch reprogram
 * (out 0x43/0x40 pair keeping the 120 Hz divisor), the EOI (out 0x20) on the non-chain path,
 * the BIOS-vector chain (far call via 0x436cc to the saved [0x7e8e4]:[0x7e914]) at ~18.2 Hz
 * (divider underflow), and the iretd. Engine-visible body: vsync_timer_tick + the chain divider
 * word[0x7e918] -= 0x3e8, += 0xf17 on underflow (1000/3863 = the 18.2-of-120 Hz ratio). */
void game_heartbeat_timer_isr(void)
{
    vsync_timer_tick();                              /* call 0x122e3 */
    uint16_t div = (uint16_t)((uint16_t)G16(VA_g_saved_int9_segment + 0x2) - 0x3e8);
    G16(VA_g_saved_int9_segment + 0x2) = div;
    if ((int16_t)div < 0)                                   /* jns skips the chain path */
        G16(VA_g_saved_int9_segment + 0x2) = (uint16_t)(div + 0xf17);
    /* the BIOS chain / EOI split is the host seam — no engine state beyond the divider */
}

/* ---- map_dos_error_to_errno (0x5612f) + body (0x56133): DOS error -> errno translation --------
 * Entry (0x5612f): `test edx,edx; je 0x5612e(ret)` — if flag(EDX)==0, return EAX unchanged with no
 * side effects; else fall into the body. EAX = DOS error code, EDX = do-map flag.
 * Body (0x56133):
 *   _doserrno = dos_err & 0xff;
 *   if dos_err >= 0x100:  errno = (dos_err >> 8) & 0xff;
 *   else:
 *     idx = dos_err & 0xff;
 *     if g_dos_major(0x7256b) >= 3:      // pre-DOS-3 skips these remaps
 *        if lowbyte==0x50: idx=0x0e; elif lowbyte>=0x22: idx=0x13; elif lowbyte>=0x20: idx=5;
 *     if idx > 0x13: idx = 0x13;
 *     errno = (int8_t) top-byte of dword table[0x75621 + idx];   // sar eax,0x18
 *   set_errno(errno); return -1;
 * The errno-map table (0x75621) + g_dos_major (0x7256b) are obj3 data (staged by the oracle image);
 * set_errno/set_doserrno are the lifts above. Pure + deterministic -> oracle-verified. */
uint32_t map_dos_error_to_errno(uint32_t dos_err, uint32_t flag)
{
    if (flag == 0)
        return dos_err;                                  /* je 0x5612e: ret, EAX = input */

    set_doserrno(dos_err & 0xffu);

    int32_t err;
    if (dos_err >= 0x100u) {
        err = (int32_t)((dos_err >> 8) & 0xffu);
    } else {
        uint32_t idx     = dos_err & 0xffu;
        uint32_t lowbyte = dos_err & 0xffu;
        if ((uint32_t)G8(VA_g_crt_memory_mode + 0x9) >= 3u) {
            if (lowbyte == 0x50u)      idx = 0x0eu;
            else if (lowbyte >= 0x22u) idx = 0x13u;
            else if (lowbyte >= 0x20u) idx = 0x05u;
            /* else idx stays == lowbyte */
        }
        if ((int32_t)idx > 0x13)                         /* signed cmp; idx is 0..0xff, positive */
            idx = 0x13u;
        int32_t tbl = *(volatile int32_t *)GADDR((VA_g_heap_free_list + 0x11) + idx);
        err = tbl >> 24;                                 /* sar eax,0x18 (arithmetic) */
    }

    set_errno((uint32_t)err);
    return 0xffffffffu;                                  /* mov eax,-1 */
}

/* ==================== cluster D tail — exception / critical-error handler pairs ==============
 * The last four dos_runtime fns. They carried inline `int 0x31` (DPMI 0202/0203 exception
 * handlers, 0204/0205 PM vectors) + `int 0x21` (AH=09 crash print) + direct real-mode
 * low-memory pokes (the RM IVT INT 24h dword at 0x90, BDA 0x43e) — all now C2 API calls
 * (os_api.h). Oracle-verified by the c2_mock differential (patched-INT servicer +
 * abs32-repointed low-memory operands feeding both sides identical canned state); the
 * DS/SS/CS reads come from the live segment registers, identical in-process on both sides. */

/* ---- install_exception_handler (0x416d3): hook the DPMI divide-error (#0) handler ------------
 * EAX = mode flags, EDX = a context dword the CRT fault path reports later. Always captures the
 * machine state (DS 0x90d28, ESP 0x90cf0, SS 0x90d14, EDX-arg 0x90ce8). If [0x727e0]==0 and
 * AL bit3: the CRT GP/page-fault installer 0x41742 (crt klass — bridged; its own int31s are
 * serviced the same way). Then, if not already installed ([0x90d1c]==0): saves the current #0
 * handler (0202) to 0x90d1e/0x90cec, installs CS:0x41a31 (0203), and flips the installed flag
 * word to 0xffff only when the set did NOT return CF (the `jb 0x41740`). */
void install_exception_handler(uint32_t eax, uint32_t edx)
{
    uint16_t seg;
    __asm__ volatile ("mov %%ds, %0" : "=r"(seg));
    G16(VA_g_flat_shading_flag + 0x64) = seg;
    uint32_t esp;
    __asm__ volatile ("mov %%esp, %0" : "=r"(esp));
    G32(VA_g_flat_shading_flag + 0x2c) = (int32_t)esp;
    __asm__ volatile ("mov %%ss, %0" : "=r"(seg));
    G16(VA_g_flat_shading_flag + 0x50) = seg;
    G32(VA_g_flat_shading_flag + 0x24) = (int32_t)edx;

    if (G32(VA_g_screen_mode_table + 0x6e) == 0 && (eax & 8u)) {
#ifndef ROTH_STANDALONE
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x41742u + OBJ_DELTA;                    /* CRT #d/#e installer (bridged) */
        call_orig(&io);
#else
        /* image-free boot: the Watcom CRT GP/#page-fault
         * (#d/#e) DPMI installer is a live-swap-only concern — under the imgfree boot faults are host-
         * side and obj1 is unmapped, so there is no game-side #d/#e handler to install. The #0 (divide)
         * hook below stays (os_dpmi_get/set_exc_handler). Compiled out. */
#endif
    }

    if ((uint16_t)G16(VA_g_flat_shading_flag + 0x58) != 0)
        return;                                          /* already installed */
    G32(VA_g_flat_shading_flag + 0x5c) = (int32_t)eax;
    uint16_t cs; uint32_t off;
    os_dpmi_get_exc_handler(0, &cs, &off);               /* int31 0202 BL=0 */
    G16(VA_g_flat_shading_flag + 0x5a) = cs;
    G32(VA_g_flat_shading_flag + 0x28) = (int32_t)off;
    __asm__ volatile ("mov %%cs, %0" : "=r"(seg));
    if (os_dpmi_set_exc_handler(0, seg, 0x41a31u + OBJ_DELTA) == 0)  /* int31 0203; jb skips */
        G16(VA_g_flat_shading_flag + 0x58) = (uint16_t)~(uint16_t)G16(VA_g_flat_shading_flag + 0x58);            /* 0 -> 0xffff */
}

/* ---- restore_exception_handler_and_report (0x41674): unhook #0 + the divide-count report -----
 * EAX = flags (bit0 = report). First the CRT #d/#e restorer 0x417a1 (bridged). If installed
 * ([0x90d1c]!=0): flag word back to 0, restore the saved #0 handler (0203 from 0x90d1e/0x90cec,
 * CF ignored), and when bit0 is set and the divide counter [0x90ce4] is nonzero, format the CRT
 * crash template 0x72578 into 0x90d44 and DOS-print it (int21 AH=09, stops at the '$').
 * QUIRK (faithful): the template holds ~24 specifiers but the caller pushes only the counter —
 * the original formats the whole template from whatever garbage sits above it on the stack; only
 * the first sentence (up to the '$') is ever observable. The lift passes zeros for those dead
 * varargs — the tail of 0x90d44 is write-only state nothing reads (the print stops at '$'). */
void restore_exception_handler_and_report(uint32_t eax)
{
    {
#ifndef ROTH_STANDALONE
        regs_t io; memset(&io, 0, sizeof io);
        io.va = 0x417a1u + OBJ_DELTA;                    /* CRT #d/#e restorer (bridged) */
        call_orig(&io);
#else
        /* image-free: CRT #d/#e DPMI restorer is live-swap-only (host owns faults; nothing
         * was installed image-free). The #0 unhook below stays (os_dpmi_set_exc_handler). */
#endif
    }
    if ((uint16_t)G16(VA_g_flat_shading_flag + 0x58) == 0)
        return;
    G16(VA_g_flat_shading_flag + 0x58) = (uint16_t)~(uint16_t)G16(VA_g_flat_shading_flag + 0x58);    /* 0xffff -> 0 */
    os_dpmi_set_exc_handler(0, (uint16_t)G16(VA_g_flat_shading_flag + 0x5a), (uint32_t)G32(VA_g_flat_shading_flag + 0x28));
    if ((eax & 1u) == 0)
        return;
    uint32_t cnt = (uint32_t)G32(VA_g_flat_shading_flag + 0x20);
    if (cnt == 0)
        return;
    uint32_t args[24];
    memset(args, 0, sizeof args);
    args[0] = cnt;
    vsprintf_core((const uint8_t *)GADDR(VA_g_crt_memory_mode + 0x16), (uint8_t *)GADDR(VA_g_flat_shading_flag + 0x80), args);
    os_dos_print_string(0x90d44u + OBJ_DELTA);           /* int21 AH=09, edx = the buffer */
}

/* ---- install_critical_error_handler (0x436e8): hook DOS INT 24h with an auto-FAIL stub -------
 * No args, void. Once ([0x911cc]==0): a 0x40-byte DOS block (alloc_dos_block — lifted), the
 * 0x28-byte real-mode stub template at 0x60000 copied to the paragraph-aligned block start
 * (stub: mov cs:[0]=0xffff, save DI to cs:[2], AL=3 "FAIL", iret — entry at offset 4), BDA
 * 0x43e zeroed, the RM IVT INT 24h dword [0x90] repointed to seg:0004 of the block (old vector
 * saved to 0x911d0), and the PM INT 24h vector swapped to CS:0x437d7 via 0204/0205 (old to
 * 0x911d8/0x911d4). The alloc-fail path stores the 0 and returns (same as the original). */
void install_critical_error_handler(void)
{
#ifdef ROTH_STANDALONE
    /* image-free: the host services all DOS file I/O through the os_dos_* contract, which reports errors
     * as CF (there is no real INT 24h), and obj2's real-mode auto-FAIL stub template at 0x60000 is not
     * mapped (original CODE, not materialized) — the game's critical-error handler is never invoked, so
     * this install is a no-op. Matches the boot-spine "no original bytes" invariant. */
    return;
#else
    if (G32(VA_g_spawn_projectile_is_player + 0x1) != 0)
        return;
    uint32_t blk = alloc_dos_block(0x40);         /* 0x35a12 (lifted) */
    G32(VA_g_spawn_projectile_is_player + 0x1) = (int32_t)blk;
    if (blk == 0)
        return;
    if (G32(VA_g_spawn_projectile_is_player + 0x1) == 0)                               /* the original re-loads + re-tests */
        return;
    uint32_t dst = ((uint32_t)G32(VA_g_spawn_projectile_is_player + 0x1) + 0xfu) & ~0xfu;
    memcpy((void *)(uintptr_t)dst, (const void *)GADDR(0x60000), 0x28);  /* rep movsd x10 */
    os_lowmem_write8(0x43e, 0);                          /* BDA: clear the floppy recal byte */
    uint32_t rmvec = ((dst << 12) & 0xffff0000u) | 4u;   /* RM far ptr seg=dst>>4 : off 4 */
    uint32_t old = os_lowmem_read32(0x90);               /* IVT slot 0x24*4 */
    os_lowmem_write32(0x90, rmvec);
    G32(VA_g_saved_int24_vector) = (int32_t)old;
    uint16_t cs; uint32_t off;
    os_dpmi_get_pm_vector(0x24, &cs, &off);              /* int31 0204 */
    G16(VA_g_saved_int24_vector + 0x8) = cs;
    G32(VA_g_saved_int24_vector + 0x4) = (int32_t)off;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    os_dpmi_set_pm_vector(0x24, cs, 0x437d7u + OBJ_DELTA);   /* int31 0205 */
#endif /* ROTH_STANDALONE */
}

/* ---- restore_critical_error_handler (0x43775): unhook INT 24h + free the stub block ----------
 * No args, void. Puts back the saved RM IVT dword (0x911d0 -> [0x90]), the saved PM vector
 * (0x911d4/0x911d8 via 0205), then frees the DOS stub block through free_os_block_guarded
 * (0x35af2 — lifted; the block global is cleared BEFORE the free, as the original). */
void restore_critical_error_handler(void)
{
    uint32_t old = (uint32_t)G32(VA_g_saved_int24_vector);
    if (old != 0) {
        os_lowmem_write32(0x90, old);
        G32(VA_g_saved_int24_vector) = 0;
    }
    if (G32(VA_g_saved_int24_vector + 0x4) != 0) {
        os_dpmi_set_pm_vector(0x24, (uint16_t)G16(VA_g_saved_int24_vector + 0x8), (uint32_t)G32(VA_g_saved_int24_vector + 0x4));
        G32(VA_g_saved_int24_vector + 0x4) = 0;
    }
    uint32_t blk = (uint32_t)G32(VA_g_spawn_projectile_is_player + 0x1);
    if (blk != 0) {
        G32(VA_g_spawn_projectile_is_player + 0x1) = 0;
        free_os_block_guarded(blk);               /* 0x35af2 (lifted) */
    }
}
