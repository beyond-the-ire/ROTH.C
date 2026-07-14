/* wraptest.c — the ROTH.C modding SDK "wraptest" example plugin (override registry).
 *
 * Proves the boot-time function-override registry end-to-end, compiling against ONLY <roth_sdk.h>
 * (no engine headers) — the same self-containment proof as hello.
 *
 *   make            # -> plugin.so  (gcc -m32 -shared -fPIC -std=c11 -Wall -Wextra -Werror)
 *   cp plugin.so <game-dir>/mods/wraptest/plugin.so   then run the host.
 *
 * It registers TWO runtime overrides in on_register_overrides(MAIN):
 *
 *   (1) dos_open_file  — a BOOT-PATH function the engine calls to open game files. The wrap logs its
 *       first few calls and forwards through roth_next_dos_open_file() to the recorded original body,
 *       returning its handle unchanged. This is the observable dispatch -> next -> base proof: the log
 *       shows the intercept firing on the real boot with behavior preserved.
 *
 *   (2) render_inspect_popup_window — the function the doc_viewer example plugin replaces
 *       (sdk/examples/doc_viewer, a full-replacement chain entry). Registering this wrap at a HIGHER
 *       priority proves chains COMPOSE: with doc_viewer also installed the chain is wraptest (outer)
 *       -> doc_viewer -> the pristine engine body; without it, wraptest -> pristine body. It fires
 *       when an inspect popup opens in-game; at boot it is simply reported by --dump-mod-chains as a
 *       resolved chain on 0x18e9e.
 */
#include <stdint.h>
#include <stdio.h>

#include "roth_sdk.h"

/* (1) boot-path, observable: log the first few opens, forward to the original. */
static uint32_t wt_dos_open_file(struct roth_chain *chain, const struct roth_api_v1 *api,
                                 uint32_t name_ptr, uint32_t mode)
{
    (void)api;
    uint32_t handle = roth_next_dos_open_file(chain, name_ptr, mode);   /* -> next link / recorded body */
    static int fired;
    if (fired < 3) {
        fired++;
        fprintf(stderr, "[wraptest] dos_open_file wrap #%d: (name_ptr=0x%x, mode=0x%x) "
                        "-> handle 0x%x  [dispatch -> roth_next -> base, return preserved]\n",
                fired, name_ptr, mode, handle);
    }
    return handle;
}

/* (2) chain composition: log + forward via roth_next (-> the doc_viewer runtime plugin's link if
 * installed at a lower priority, else the pristine engine body). */
static uint32_t wt_render_inspect_popup_window(struct roth_chain *chain, const struct roth_api_v1 *api,
                                               uint32_t width, uint32_t height,
                                               uint32_t node, uint32_t p4)
{
    (void)api;
    fprintf(stderr, "[wraptest] render_inspect_popup_window wrap fired (outer link; forwarding via "
                    "roth_next to the next chain entry / pristine body)\n");
    return roth_next_render_inspect_popup_window(chain, width, height, node, p4);
}

static void on_register_overrides(const struct roth_api_v1 *api, struct roth_registrar_v1 *reg)
{
    (void)api;
    /* priority 100; a single plugin so no chain-ordering conflict. Returns 0 on success. */
    int r1 = roth_override(reg, ROTH_FN_dos_open_file, wt_dos_open_file, 100);
    int r2 = roth_override(reg, ROTH_FN_render_inspect_popup_window,
                           wt_render_inspect_popup_window, 100);
    fprintf(stderr, "[wraptest] on_register_overrides: dos_open_file=%d "
                    "render_inspect_popup_window=%d (0 = accepted)\n", r1, r2);
}

static void on_load(const struct roth_api_v1 *api)
{
    fprintf(stderr, "[wraptest] on_load (MAIN): host api abi %u.%u\n", api->abi_major, api->abi_minor);
}

static const struct roth_plugin_info_v1 WRAPTEST = {
    .abi_major     = ROTH_ABI_MAJOR,
    .abi_minor     = ROTH_ABI_MINOR,
    .struct_size   = sizeof(struct roth_plugin_info_v1),
    .id            = "ire.roth.examples.wraptest",
    .name          = "Wraptest (override-registry example)",
    .version       = "1.0.0",
    .sdk_req_major = ROTH_SDK_MAJOR,
    .sdk_req_minor = ROTH_SDK_MINOR,
    .api_use       = ROTH_API_USE_ENGINE,
    .on_load              = on_load,
    .on_game_ram_ready    = NULL,
    .on_frame_game        = NULL,
    .on_compose_tick      = NULL,
    .on_audio             = NULL,
    .on_unload            = NULL,
    .on_register_overrides = on_register_overrides,
};

ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void)
{
    return &WRAPTEST;
}
