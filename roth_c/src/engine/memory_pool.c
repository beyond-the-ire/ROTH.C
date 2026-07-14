/* lift_memory_pool.c — the game-heap / 'Pool' handle allocator wrappers lifted to verified C.
 * Split out of renderer.c (per docs/operating/recomp.md §4.6: every subsystem gets its own TU).
 *
 * memory_pool is the foundational allocator under every dynamic allocation in ROTH: the game heap
 * (g_game_heap_handle 0x7f374) and the relocatable 'Pool'-magic handle allocator (g_resource_pool
 * 0x85c40). The Pool core (pool_init/create/alloc/free/coalesce, game_heap_alloc/free,
 * block_payload_size) is already lifted (reached via engine.h); this TU adds the thin wrappers, the
 * heap-grow helper, and the integrity walker.
 *
 * A4 — STORED POINTERS: g_game_heap_handle (0x7f374) and g_resource_pool (0x85c40) hold runtime
 * handle values (relocatable chunk pointers). They live in canon obj3, so GADDR(va) gives the
 * *global's* runtime address; the VALUE read from it is the handle (a raw runtime pointer) — pass it
 * straight to the Pool primitives, never add OBJ_DELTA to a handle. ABI from the DISASM.
 * lift-lens: docs/reference/lift/memory_pool.md.
 */
#include "common.h"
#include "engine.h"
#include "os_api.h"
#include <string.h>

/* read a stored handle (a runtime pointer value) out of a canon obj3 global */
#define HANDLE(canon_va) (*(volatile uint32_t *)(uintptr_t)GADDR(canon_va))

/* ====================================================================== Layer 1 — leaves */

/* query_game_heap_free (0x151a5, 10 B) — `mov eax,[g_game_heap_handle]; jmp block_payload_size`.
 * Tail-call: returns block_payload_size(g_game_heap_handle) = the heap's free payload size. */
uint32_t query_game_heap_free(void)
{
    return block_payload_size(HANDLE(0x7f374));
}

/* game_free_if_not_null (0x40a2a, 10 B) — null-safe `if (block) game_heap_free(block)`. EAX=block. */
void game_free_if_not_null(uint8_t *block)
{
    if (block) game_heap_free(block);
}

/* game_heap_alloc_round4 (0x40a17, 19 B) — round the size up to a 4-byte multiple, then
 * game_heap_alloc. `clc` on success is a no-op (the preceding `or eax,eax` already cleared CF).
 * EAX=size -> EAX=allocated ptr (the corpus drops this return; the real callers consume it). */
uint32_t game_heap_alloc_round4(int32_t size)
{
    uint32_t rounded = ((uint32_t)size + 3) & 0xfffffffcu;   /* add eax,3; and eax,~3 */
    return game_heap_alloc((int32_t)rounded);
}

/* alloc_resource_pool_block (0x26b38, 20 B) — pool_alloc_checked(g_resource_pool, size). EAX=size. */
uint32_t alloc_resource_pool_block(int32_t size)
{
    return pool_alloc_checked(HANDLE(0x85c40), size);
}

/* free_resource_chunk (0x26b4c, 20 B) — pool_free_chunk(g_resource_pool, block). EAX=block. */
uint32_t free_resource_chunk(uint8_t *block)
{
    return pool_free_chunk((uint32_t *)(uintptr_t)HANDLE(0x85c40), block);
}

/* init_resource_chunk_pool (0x35b53, 21 B) — `if (block) pool_init(block, 0x18, size)`.
 * EAX=block, EDX=size. The null path returns EAX (== 0). */
uint32_t init_resource_chunk_pool(uint32_t *block, int32_t size)
{
    if (block == 0) return 0;
    return pool_init(block, 0x18, size);
}

/* free_block_or_pool (0x15280, 16 B) — `cmp eax,0x100000; jae game_heap_free; jmp free_os_block_guarded`.
 * High addresses (>= 1 MB = the DPMI-backed heap/pool) free through the game heap; low addresses
 * (< 1 MB = DOS conventional blocks) tail into the dos_runtime guard — lifted, called direct
 * (re-pointed from the 0x35af2 call_orig bridge once dos_runtime closed). EAX=block. */
void free_block_or_pool(uint8_t *block)
{
    if ((uint32_t)(uintptr_t)block >= 0x100000u)
        game_heap_free(block);                       /* tail -> game_heap_free */
    else
        free_os_block_guarded((uint32_t)(uintptr_t)block);   /* tail -> 0x35af2 */
}

/* get_pool_descriptor (0x35f04, 170 B) — a debug-gated Pool-integrity walker. When
 * g_pool_check_enabled (0x8a278) == 0 it returns the pool pointer unchanged (the production case —
 * 0x8a278 has NO writers in the image, so the walk is inert). When enabled it walks the chunk list
 * read-only, validating magic / prev-links / flag bits / allocated-chunk handle back-refs /
 * free-list max-free + total-free consistency / last-chunk flag / the backing block-header size, and
 * `int3`s (debug break) on any inconsistency. It mutates nothing and returns EAX = the input pool.
 * Faithful read-only transcription; the int3 failure path is reproduced as __builtin_trap(). */
uint32_t get_pool_descriptor(uint32_t eax)
{
    if (G32(VA_g_pool_check_enabled) == 0) return eax;                      /* 0x35f04: disabled -> pass-through */

    const uint32_t pool = eax;
    uint32_t ebx = 0, edx = 0, ebp = 0, edi = 0;            /* max-free, prev-chunk, total-free, htab */
    uint32_t esi = eax;
    if (*(volatile uint32_t *)(uintptr_t)esi != 0x506f6f6cu) __builtin_trap();   /* 'Pool' magic */
    if (*(volatile uint32_t *)(uintptr_t)(esi + 0xc) != 0x18)
        edi = esi + 0x18;                                  /* hdrsize != 0x18 -> handle table present */
    uint32_t ecx = *(volatile uint32_t *)(uintptr_t)(esi + 0x14);   /* chunk count */
    if (ecx == 0) __builtin_trap();
    esi += *(volatile uint32_t *)(uintptr_t)(esi + 0xc);    /* first chunk = pool + hdrsize */

    for (;;) {                                              /* 0x35f3b — per chunk */
        if ((int32_t)*(volatile uint32_t *)(uintptr_t)(esi + 4) <= 0) __builtin_trap();   /* size > 0 */
        if (*(volatile uint32_t *)(uintptr_t)esi != edx) __builtin_trap();                /* prev-link */
        if (*(volatile uint16_t *)(uintptr_t)(esi + 8) & 0xffe0) __builtin_trap();        /* hi flags clear */
        if (*(volatile uint8_t *)(uintptr_t)(esi + 8) & 1) {           /* allocated chunk */
            if (edi != 0) {                                            /* validate handle back-ref */
                uint32_t cx = *(volatile uint16_t *)(uintptr_t)(esi + 0xa);   /* handle index */
                uint32_t slot = pool + cx * 4 + 0x18;
                if (*(volatile uint32_t *)(uintptr_t)slot != esi + 0x10) __builtin_trap();
            }
        } else {                                                       /* free chunk -> accumulate */
            uint32_t sz = *(volatile uint32_t *)(uintptr_t)(esi + 4);
            if (sz >= ebx) ebx = sz;                                   /* max-free (unsigned) */
            ebp += sz;                                                 /* total-free */
        }
        edx = esi;                                                     /* prev = current */
        esi += *(volatile uint32_t *)(uintptr_t)(esi + 4);             /* advance to next chunk */
        ecx--;
        if (!((int32_t)ecx > 0)) break;                                /* jg */
    }

    if (*(volatile uint32_t *)(uintptr_t)(pool + 8) != ebp) __builtin_trap();   /* total-free match */
    if (*(volatile uint32_t *)(uintptr_t)(pool + 4) != ebx) __builtin_trap();   /* max-free match */
    if (!(*(volatile uint32_t *)(uintptr_t)(edx + 8) & 2)) __builtin_trap();    /* last-chunk flag */
    esi -= pool;                                                       /* total span walked */
    if (*(volatile uint32_t *)(uintptr_t)(pool - 8) != esi) {          /* backing-block size check */
        esi += 0x10;
        if (*(volatile uint32_t *)(uintptr_t)(pool - 0xc) != esi) __builtin_trap();
    }
    return pool;                                                       /* popal restores EAX=pool */
}

/* ====================================================================== Layer 2 / 3 */

/* small bridge into an un-lifted callee (no args read beyond the regs we set) */
static uint32_t mp_bridge_eax(uint32_t canon_va, uint32_t eax)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon_va + OBJ_DELTA; io.eax = eax;
#ifndef ROTH_STANDALONE
    call_orig(&io);
    return io.eax;
#else
    switch (canon_va) {   /* M3 routes (0x56ce5 = CRT heap-grow helper, no lifted body — stays fail-loud) */
    case 0x41b41u: dos_close_handle(eax); return 0;
    case 0x26cd4u: flush_object_das_handles(); return 0;
    case 0x30162u: free_audio_stream_buffers(); return 0;
    default: break;
    }
    roth_unreachable(canon_va);   /* un-lifted CRT heap-grow helper — off the bare-title path */
    return 0;
#endif
}

/* compute_heap_grow_size (0x57002, 125 B) — round a requested grow size (read from *EAX) up to an
 * 8-byte multiple, adjust by the CRT memory mode (g@0x72562; modes 1[/w 0x72563==0] and 9 add 8,
 * else subtract the selector-limit helper FUN_00056ce5's result), clamp up to g@0x758a8, then
 * page-align (clear the low 12 bits). Writes the result back to *EAX at each stage; returns 1 on a
 * nonzero result, 0 on a zero request or an unsigned overflow. EAX = ptr -> bool in EAX.
 * (FUN_00056ce5 is a CRT helper [selector-limit math] -> bridged; the oracle exercises the add-8
 * modes which skip it.) */
uint32_t compute_heap_grow_size(uint32_t *ptr)
{
    uint32_t edx = *ptr + 0xb;                  /* mov edx,[eax]; add edx,0xb */
    edx &= 0xfffffff8u;                         /* and dl,0xf8 (round down to *8) */
    if (edx == 0) return 0;                     /* test edx; je -> ret 0 (no *ptr write) */

    uint8_t mode = G8(VA_g_crt_memory_mode);
    int add8 = (mode == 1 && G8(VA_g_crt_memory_mode + 0x1) == 0) || (mode == 9);
    if (add8) {
        edx += 8;                               /* 0x5703a */
    } else {
        edx -= mp_bridge_eax(0x56ce5, 0);       /* 0x5703f call FUN_00056ce5; sub edx,eax */
    }
    *ptr = edx;                                 /* 0x57046 */
    if (edx + 0x3c < edx) return 0;             /* add edx,0x3c; jb -> ret 0 (*ptr keeps edx) */
    edx += 0x3c;

    uint32_t lim = (uint32_t)G32(VA_g_heap_free_list + 0x298);      /* mov esi,[0x758a8] */
    if (edx < lim) { edx = lim & 0xfffffffeu; } /* clamp up; and dl,0xfe (even) */
    *ptr = edx;                                 /* 0x5705e */
    if (edx + 0xfff < edx) return 0;            /* add edx,0xfff; jb -> ret 0 (*ptr keeps edx) */
    edx += 0xfff;
    edx &= 0xfffff000u;                         /* xor dl,dl; and dh,0xf0 (4 KB page align) */
    *ptr = edx;                                 /* 0x5706f */
    return edx != 0 ? 1u : 0u;                  /* setne al */
}

/* free_resource_buffers (0x26c8c, 72 B) — resource/sound teardown: close the sound-bank file handle
 * (g@0x848f8 via dos_close_handle), free the sample table (g@0x848f4 via flush_object_das_handles +
 * game_heap_free), then ALWAYS tail into free_audio_stream_buffers (g@0x30162, audio) which frees the
 * voice-stream double-buffers + the resource pool. dos_close_handle / flush_object_das_handles /
 * free_audio_stream_buffers belong to other subsystems -> bridged via call_orig (their effects are
 * identical whether reached by the original's tail-jmp or this bridge). game_heap_free is lifted. */
void free_resource_buffers(void)
{
    if (G32(VA_g_sound_bank_file_handle) != 0) {                                 /* g_sound_bank_file_handle */
        mp_bridge_eax(0x41b41, (uint32_t)G32(VA_g_sound_bank_file_handle));      /* dos_close_handle(handle) */
        G32(VA_g_sound_bank_file_handle) = 0;
    }
    if (G32(VA_g_sound_sample_table) != 0) {                                 /* g_sound_sample_table */
        mp_bridge_eax(0x26cd4, 0);                           /* flush_object_das_handles() */
        game_heap_free((uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_sound_sample_table));
        G32(VA_g_sound_sample_table) = 0;
    }
    mp_bridge_eax(0x30162, 0);                               /* tail -> free_audio_stream_buffers */
}

/* free_heap_block (0x35bfa, 9 B) — `if (block) free_os_block(block)`. free_os_block (0x35b0a) is a
 * dos_runtime function (walks g_block_list_head + DPMI-frees) -> bridged. The null guard is the only
 * non-bridge logic (oracle-verified for block==0; non-null is identical by construction). EAX=block. */
void free_heap_block(uint8_t *block)
{
    if (block) free_os_block((uint32_t)(uintptr_t)block);   /* re-pointed: was mp_bridge 0x35b0a */
}

/* alloc_block_or_heap (0x15210, 29 B) — try a DOS conventional-memory block first (alloc_dos_block,
 * now lifted C over the C2 API); on failure (0) fall back to game_heap_alloc. EAX=size -> EAX=ptr. */
uint32_t alloc_block_or_heap(int32_t size)
{
    uint32_t blk = alloc_dos_block((uint32_t)size);   /* re-pointed: was mp_bridge 0x35a12 */
    if (blk != 0) return blk;
    return game_heap_alloc(size);                     /* heap fallback */
}

/* alloc_largest_heap_block (0x35ff9, 140 B) — STARTUP: query DPMI free memory (INT 31h AX=0500h),
 * size the largest block, allocate it via alloc_dpmi_block in a shrink-by-4K retry loop, hand it to
 * init_resource_chunk_pool (lifted), then free the optional probe block. EAX = in.
 *
 * C2-CLOSED: the inline `int 0x31` is now the C2 call
 * os_dpmi_get_free_mem_info(); the alloc_dpmi_block / free_os_block bridges are the LIFTED
 * allocators (lift_dos_runtime.c cluster C) — zero traps, zero call_orig in this function.
 * Verified by the oracle differential against the ORIGINAL with its int31 sites serviced from
 * the same canned mock (c2_mock.c; test in test_dos_runtime.c). */
uint32_t alloc_largest_heap_block(uint32_t eax_in)
{
    G32(VA_g_pool_check_enabled + 0x4) = 0;                                        /* mov [0x8a27c],0 */

    uint32_t probe = 0;
    if (eax_in != 0)                                         /* or eax,eax; je -> skip */
        probe = alloc_dpmi_block(eax_in);            /* initial probe block */

    uint8_t info[0x40];
    os_dpmi_get_free_mem_info(info);                        /* int 31h AX=0500h -> C2 call */

    uint32_t ecx = *(uint32_t *)(info + 0);                  /* mov ecx,[edi] — largest bytes */
    uint32_t esi = 0x8000;
    if ((int32_t)*(uint32_t *)(info + 8) > 0) {              /* cmp [edi+8],0; jle skip */
        ecx = *(uint32_t *)(info + 8) << 0xc;                /* pages -> bytes */
        esi = 0;
        uint32_t cap = (uint32_t)G32(VA_g_pool_check_enabled + 0x4);
        if (!(ecx > cap)) {                                  /* ja skip (else clamp by 0x8a27c) */
            ecx = probe;
            if (!(probe < cap))                              /* jb skip */
                ecx = cap;
        }
    }
    ecx -= esi;
    ecx -= 0x14;                                             /* header */

    uint32_t block;
    for (;;) {                                               /* shrink-by-4K retry */
        block = alloc_dpmi_block(ecx);
        if (block != 0) break;
        ecx -= 0x1000;
    }

    uint32_t pool = init_resource_chunk_pool((uint32_t *)(uintptr_t)block, (int32_t)ecx);

    if (probe != 0)                                          /* free the probe block */
        free_os_block(probe);

    return pool;
}
