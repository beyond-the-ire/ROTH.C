/* host_adapters.c — GENERATED host-adapter table (tools/sdk_gen.py).
 *
 * Provides the host-side API table plugins call through: the engine-call table
 * (each slot bound to the real engine.h function) plus the bounded little-endian
 * game_ram accessors. Binding each engine slot to its engine.h symbol is also a
 * COMPILE-TIME ABI PROOF — if a generated wrapper type disagreed with engine.h,
 * this file would not compile.
 *
 * Standalone build check: gcc -m32 -std=c11 -Wall -Wextra -c \
 *   -Isdk/include -Iroth_c/src/engine sdk/generated/host_adapters.c
 */
#include <stdint.h>
#include <string.h>
#include "roth_sdk.h"
#include "engine.h"   /* the real engine prototypes: slot = &fn must type-check */

/* game_ram base+size are provided by the host at link time. */
extern uint8_t  *roth_game_ram_base;
extern uint32_t  roth_game_ram_size;

/* ---- bounded little-endian game_ram accessors ---------------------------- */
static uint8_t gr_u8(uint32_t off) {
    return (off < roth_game_ram_size) ? roth_game_ram_base[off] : (uint8_t)0;
}
static uint16_t gr_u16(uint32_t off) {
    if ((uint64_t)off + 2 > roth_game_ram_size) return 0;
    const uint8_t *p = roth_game_ram_base + off;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t gr_u32(uint32_t off) {
    if ((uint64_t)off + 4 > roth_game_ram_size) return 0;
    const uint8_t *p = roth_game_ram_base + off;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void gr_set_u8(uint32_t off, uint8_t v) {
    if (off < roth_game_ram_size) roth_game_ram_base[off] = v;
}
static void gr_set_u16(uint32_t off, uint16_t v) {
    if ((uint64_t)off + 2 > roth_game_ram_size) return;
    uint8_t *p = roth_game_ram_base + off;
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void gr_set_u32(uint32_t off, uint32_t v) {
    if ((uint64_t)off + 4 > roth_game_ram_size) return;
    uint8_t *p = roth_game_ram_base + off;
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static int gr_read_block(uint32_t off, void *dst, uint32_t n) {
    if ((uint64_t)off + n > roth_game_ram_size) return -1;
    memcpy(dst, roth_game_ram_base + off, n); return 0;
}
static int gr_write_block(uint32_t off, const void *src, uint32_t n) {
    if ((uint64_t)off + n > roth_game_ram_size) return -1;
    memcpy(roth_game_ram_base + off, src, n); return 0;
}
static uint32_t gr_size(void) { return roth_game_ram_size; }
/* resolve a canonical VA to its host pointer (base+off); NULL out of range. VA->host-ptr op. */
static void *gr_to_ptr(uint32_t off) {
    return (off < roth_game_ram_size) ? (void *)(roth_game_ram_base + off) : (void *)0;
}

static const struct roth_game_ram_api_v1 GR = {
    .u8 = gr_u8,
    .u16 = gr_u16,
    .u32 = gr_u32,
    .set_u8 = gr_set_u8,
    .set_u16 = gr_set_u16,
    .set_u32 = gr_set_u32,
    .read_block = gr_read_block,
    .write_block = gr_write_block,
    .size = gr_size,
    .to_ptr = gr_to_ptr,
};

/* ---- engine-call table: slot = the real engine.h symbol (the ABI proof) --- */
static const struct roth_engine_api_v1 ENG = {
    .clear_framebuffer_rect = clear_framebuffer_rect,
    .save_framebuffer_region = save_framebuffer_region,
    .blit_item_icon = blit_item_icon,
    .draw_text_to_buffer = draw_text_to_buffer,
    .register_dirty_rect = register_dirty_rect,
    .screen_xy_to_framebuffer_ptr = screen_xy_to_framebuffer_ptr,
    .load_item_icon_resource = load_item_icon_resource,
    .copy_record_block_op7 = copy_record_block_op7,
    .load_das_cache_resource = load_das_cache_resource,
    .blit_das_image_auto_scale = blit_das_image_auto_scale,
    .blit_reloc_das_image = blit_reloc_das_image,
    .draw_text_at_screen_xy = draw_text_at_screen_xy,
    .blit_das_image_at_xy = blit_das_image_at_xy,
    .resolve_reloc_record_fields = resolve_reloc_record_fields,
    .give_item = give_item,
    .read_next_dialogue_line = read_next_dialogue_line,
    .measure_control_text_width = measure_control_text_width,
    .build_available_choice_menu = build_available_choice_menu,
    .resolve_reloc_ptr = resolve_reloc_ptr,
    .pool_free_handle = pool_free_handle,
    .dos_open_file = dos_open_file,
    .dos_close_handle = dos_close_handle,
    .blit_das_image_to_buffer = blit_das_image_to_buffer,
    .draw_popup_shadow_border_smc = draw_popup_shadow_border_smc,
};

/* host services on the top-level table: provided by the host loader (extern here — the
 * standalone -c ABI proof leaves them unresolved; the standalone game link binds plugin_loader.o). */
extern const char *roth_sdk_current_plugin_dir(void);

ROTH_SDK_EXPORT const struct roth_api_v1 roth_host_api_v1 = {
    .abi_major = ROTH_ABI_MAJOR,
    .abi_minor = ROTH_ABI_MINOR,
    .struct_size = (uint32_t)sizeof(struct roth_api_v1),
    .engine = &ENG,
    .game_ram = &GR,
    .plugin_dir = roth_sdk_current_plugin_dir,
};
