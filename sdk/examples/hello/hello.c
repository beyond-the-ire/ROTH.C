/* hello.c — the ROTH.C modding SDK "hello" example plugin (the ABI self-containment proof).
 *
 * This file compiles against ONLY <roth_sdk.h> — no engine.h, no repo headers, nothing from the host
 * tree. If it builds and loads, the SDK header is genuinely self-contained.
 *
 *   make            # -> plugin.so  (gcc -m32 -shared -fPIC -std=c11 -Wall -Wextra -Werror)
 *
 * Install for testing:  cp plugin.so <game-dir>/mods/hello/plugin.so   then run the host.
 *
 * It exercises every v1 lifecycle context:
 *   on_load           (MAIN)     — after validation, before any engine thread starts
 *   on_game_ram_ready (MAIN)     — reads a known game_ram value through the bounded accessors
 *   on_frame_game     (GAME)     — first-fire log, proving the GAME-thread per-tick dispatch
 *   on_compose_tick   (TICK_ISR) — draws a fixed 8x8 marker into the frame (screenshot proof)
 *   on_unload         (MAIN)     — teardown
 */
#include <stdint.h>
#include <stdio.h>

#include "roth_sdk.h"

/* Overridable for the loader's negative tests (a canonical build uses the real values). */
#ifndef HELLO_ID
#define HELLO_ID "ire.roth.examples.hello"
#endif
#ifndef HELLO_ABI_MAJOR
#define HELLO_ABI_MAJOR ROTH_ABI_MAJOR
#endif

/* The 70Hz engine frame-tick counter — a stable, well-known game_ram value (canonical VA). */
#define VA_FRAME_TICK 0x90bccu

static void on_load(const struct roth_api_v1 *api)
{
    fprintf(stderr, "[hello] on_load (MAIN): host api abi %u.%u, struct_size=%u\n",
            api->abi_major, api->abi_minor, api->struct_size);
}

static void on_game_ram_ready(const struct roth_api_v1 *api)
{
    /* Read a known game_ram value through the bounded little-endian accessors. Pre-threads the tick
     * counter reads 0 (game_ram staged pristine) — the point is that the accessor path works. */
    uint16_t tick = api->game_ram->u16(VA_FRAME_TICK);
    fprintf(stderr, "[hello] on_game_ram_ready (MAIN): game_ram size=%u bytes, "
                    "frame_tick@0x90bcc=%u\n", api->game_ram->size(), tick);
}

static void on_frame_game(const struct roth_api_v1 *api)
{
    static int fired;
    if (fired)
        return;
    fired = 1;
    fprintf(stderr, "[hello] on_frame_game (GAME): first fire, frame_tick@0x90bcc=%u\n",
            api->game_ram->u16(VA_FRAME_TICK));
}

static void on_compose_tick(const struct roth_api_v1 *api, uint8_t *pixels,
                            uint32_t width, uint32_t height)
{
    (void)api;   /* TICK_ISR (restricted): composite only — NO engine calls here. */
    if (!pixels || width < 8 || height < 8)
        return;
    /* A solid 8x8 marker in the top-left corner, one fixed palette index — visible screenshot proof. */
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++)
            pixels[y * width + x] = 0xE8;
}

static void on_unload(const struct roth_api_v1 *api)
{
    (void)api;
    fprintf(stderr, "[hello] on_unload (MAIN)\n");
}

static const struct roth_plugin_info_v1 HELLO = {
    .abi_major     = HELLO_ABI_MAJOR,
    .abi_minor     = ROTH_ABI_MINOR,
    .struct_size   = sizeof(struct roth_plugin_info_v1),
    .id            = HELLO_ID,
    .name          = "Hello (SDK example)",
    .version       = "1.0.0",
    .sdk_req_major = ROTH_SDK_MAJOR,
    .sdk_req_minor = ROTH_SDK_MINOR,
    .api_use       = ROTH_API_USE_GAME_RAM | ROTH_API_USE_COMPOSE,
    .on_load           = on_load,
    .on_game_ram_ready = on_game_ram_ready,
    .on_frame_game     = on_frame_game,
    .on_compose_tick   = on_compose_tick,
    .on_audio          = NULL,   /* not interested */
    .on_unload         = on_unload,
};

ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void)
{
    return &HELLO;
}
