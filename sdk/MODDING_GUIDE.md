# The ROTH.C Modding Guide

*For mod authors. The SDK machinery underneath — schema, generators, CI gates — is documented in
[`sdk/README.md`](README.md). This guide is the practical "how do I write a mod" document.*

ROTH.C is a byte-verified recompilation of the 1996 DOS game *Realms of the Haunting*. Its engine
source is a preservation artifact — it never changes for modding. Instead, the shipped **moddable**
binary carries a runtime plugin platform: mods are folders you drop next to the game, loaded at
boot. (A separate **vanilla** build flavor has no mod surface at all — no pads, no loader — for
purists and reference runs.)

```
ROTH/                      # the game install
  roth                     # the moddable binary (default build)
  mods/
    my_mod/
      plugin.so            # your compiled mod — the only required file
      my_mod.cfg           # anything else is your business (cfg, assets, ...)
```

Plugins load at boot in **lexicographic folder order**, are fully validated before any mod code is
called back, and stay loaded until exit. `mod.toml` is reserved for future packaging metadata —
the loader currently reads only the binary. Kill switches: `--no-mods` (or `ROTH_MODS=0`) boots
with the platform inert; `--list-mods` prints what would load and exits; `--strict-mods` turns
validation warnings into hard aborts; `--dump-mod-chains` prints the resolved override chains.

## The anatomy of a plugin

A plugin is a 32-bit shared library (`gcc -m32 -shared -fPIC`) compiled against **one header**,
`roth_sdk.h` — no engine headers, no repo internals. It exports exactly one symbol:

```c
#include "roth_sdk.h"

ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void)
{
    static const struct roth_plugin_info_v1 info = {
        .abi_major   = ROTH_ABI_MAJOR,
        .abi_minor   = ROTH_ABI_MINOR,
        .struct_size = sizeof info,
        .id          = "com.example.mymod",     /* reverse-DNS, unique — this IS your identity */
        .name        = "My Mod",
        .version     = "1.0.0",
        .api_use     = ROTH_API_USE_DATA,       /* declared API use — see honesty note below */
        .on_load     = my_on_load,              /* any callback may be NULL = not interested */
        /* ... */
    };
    return &info;
}
```

**Your `id` is your permanent identity.** Pick a reverse-DNS prefix that identifies *you* and put
the mod's name after it. If you have no domain of your own, the recommended default is
`io.github.<your-username>.<mod_name>`. Never publish under a prefix that isn't yours, and note that
`ire.roth.*` is reserved for the engine's own first-party mods. The `id` is frozen for the life of
the mod — the platform keys load records and override chains off it — so renaming it doesn't update a
mod, it creates a *different* one. Choose it once and keep it.

The load rule: a plugin loads iff its `abi_major` equals the host's and its `abi_minor` is ≤ the
host's. The SDK grows append-only, so a mod built against SDK 0.4 keeps working on every 0.x host
from 0.4 up.

Every callback receives the immutable host API table `const struct roth_api_v1 *api` — your
gateway to everything: `api->engine->…` (engine calls), `api->game_ram->…` (memory access),
`api->plugin_dir()` (your folder's path, for reading your own cfg/assets during load callbacks).

## The four kinds of mods

One plugin can be several of these at once; they're capability rungs, not exclusive categories.
Each rung trades version-durability for power — use the highest rung that expresses your idea.

| type | touches | engine code | durability | example in `sdk/examples/` |
|---|---|---|---|---|
| **data** | memory at boot | runs pristine | bulletproof | `hello` |
| **overlay** | input/frames at the edges | runs pristine | very high | `keybinds`, `item_grabber` |
| **wrap** | around a function | runs, decorated | high | `wraptest` |
| **replacement** | a function's whole body | replaced by yours | pinned to engine release | `doc_viewer` |

### Data mods

Implement `on_game_ram_ready` — called at boot when the engine's entire global state (`game_ram`,
the flat image of the original EXE's memory at its 1996 addresses) is staged pristine and nothing
has run yet — and edit values through the bounded accessors:

```c
static void my_ram_ready(const struct roth_api_v1 *api) {
    uint16_t hp = api->game_ram->u16(0x89f00);      /* offsets are the frozen 1996 addresses */
    api->game_ram->set_u16(0x89f00, hp * 2);
}
```

*Simply:* the game's memory is a giant spreadsheet whose cell addresses haven't changed since 1996
and never will. A data mod edits some cells at launch, before the game reads them. No machinery is
touched, which is why data mods survive every engine update.

Rules: reads/writes are bounds-checked (out-of-range reads return 0, writes are dropped, block ops
return −1). **Never cache a pool-derived pointer across an engine call or a tick** — the DAS pool
allocator relocates its chunks; re-resolve through the handle each time. `game_ram->to_ptr(off)`
converts a canonical address to a host pointer when an engine call needs one.

### Overlay mods (seam mods)

Subscribe to the host's seam callbacks — events at the boundary between the engine and the world:

- `on_scancode` (**TICK_ISR** context): every keystroke *before* the engine sees it. Return the
  (possibly rewritten) scancode, or `0` to swallow it. Keep it instant; no engine calls here.
- `on_mouse_poll` (**GAME**): inspect/modify the mouse state each time the engine polls it.
- `on_frame_game` (**GAME**): once per engine tick on the game thread; engine calls are legal.
- `on_compose_tick` (**TICK_ISR**): the finished frame's pixel buffer just before display — write
  palette-index pixels directly to draw on top. No engine calls.

*Simply:* an overlay mod sits between you and the game. It can see and change every key before the
game does, paint on every frame after the game draws it, and — from GAME context — reach in and
pull levers (`api->engine->give_item(...)`). The `item_grabber` example is a whole item-browser UI
built from nothing but these seams; the engine underneath never changed.

**Timing law:** clock any repeat/animation off the engine tick counter
(`api->game_ram->u16(0x90bcc)`, 70 Hz, wrap-safe 16-bit compare) — never off callback counts;
poll rates vary several-fold between scenes.

### Function overrides: wraps and replacements

The moddable binary compiles every engine function with a 5-byte NOP pad at its entry. At boot —
before any game thread exists — the registry patches the pads of *overridden functions only* with
a jump into your chain (no plugins ⇒ every pad verifiably stays NOP). Register during the
`on_register_overrides` callback:

```c
static void my_register(const struct roth_api_v1 *api, struct roth_registrar_v1 *reg) {
    roth_override(reg, ROTH_FN_give_item, my_give_item, /*priority*/ 100);
}
```

Function identity is the **permanent numeric ID** (the function's address in the original 1996
binary — a historical fact that can never change); `ROTH_FN_<name>` enumerators name them. Typed
per-function callback signatures and `roth_next_<fn>` helpers are generated in
`roth_sdk_overrides.h` (included by `roth_sdk.h`).

Your callback receives the chain handle, the api table, then the original arguments. The single
decision that defines your mod:

- **Wrap — call `roth_next`:** the original still runs; you decorate it. Modify arguments before,
  modify the result after, add side effects. Wraps from any number of mods **stack**.
- **Replacement — never call `roth_next`:** you provide the complete behavior, typically a modified
  copy of the function's real C source (this project has it — a luxury no other scene gets). Full
  power, full responsibility: you own every side effect the original had, you block lower-priority
  overrides on that function, and you carry a frozen fork — engine fixes to that function don't
  reach your copy.
- **Conditional replacement** (the `doc_viewer` idiom): decide per call. Disabled ⇒ `return
  roth_next(...)` (perfect passthrough); enabled ⇒ run your modified copy.

**Chains and conflicts.** All overrides on one function form a single chain: **higher priority =
outer** (runs first); ties break by load order (duplicate priorities are rejected under
`--strict-mods`). Wraps compose; replacements are exclusive — the outermost replacement wins and
inner links never run. That's information-theoretic, not a platform choice: two compiled function
bodies can't be merged. The ecosystem answer is a *compatibility patch*: a third, higher-priority
mod that replaces the function once with both changes merged at source level by a human.
`--dump-mod-chains` shows exactly who resolved where.

**Guideline: wrap unless you truly can't.**

**Eligibility.** ~1,079 of the engine's typed functions are overrideable — the generated manifest
`sdk/abi/override_targets.json` is the authority. Eleven are hard-blocked (the timer/ISR bodies and
audio callbacks — the game's pulse; overriding them breaks timing, not gameplay). Nearly every
eligible function carries the `tick_isr_reachable` flag, which is a *contract, not a warning off*:
your override runs in whatever context the original ran — in this engine that includes the 70 Hz
timer tick, exactly as the original code has run since 1996. Keep your override roughly as fast and
self-contained as the function it replaces; defer heavy work to your own `on_frame_game`. A few
renderer functions carry `self_modifying` — their bodies contain the original's self-modifying-code
patterns; replace them only if you understand that machinery.

## Execution contexts (the law)

| context | where | you may |
|---|---|---|
| `MAIN` | boot, pre-threads (`on_load`, `on_game_ram_ready`, `on_register_overrides`, `on_unload`) | anything: file I/O, allocation, game_ram |
| `GAME` | the game thread (`on_frame_game`, `on_mouse_poll`, most overrides' call sites) | engine calls + game_ram |
| `TICK_ISR` | the timer tick (`on_scancode`, `on_compose_tick`, overrides reached from the tick) | game_ram + pixels; **no engine calls**, be fast |
| `AUDIO` | reserved (`on_audio` is never dispatched in v1) | — |

## Building

Copy an example's `Makefile` — the whole recipe is:

```
gcc -m32 -shared -fPIC -std=c11 -Wall -Wextra -Werror -I<sdk>/include -o plugin.so my_mod.c
```

The same source builds the Windows plugin with the MinGW i686 cross-compiler — drop `-fPIC`
(it is ELF-only; PE code relocates on its own):

```
i686-w64-mingw32-gcc -shared -std=c11 -Wall -Wextra -Werror -I<sdk>/include -o plugin.dll my_mod.c
```

No `dllexport` annotations or `.def` files are needed in your code — `roth_sdk.h`'s
`ROTH_PLUGIN_EXPORT` handles the entry-point export on both platforms. The engine loads
`plugin.so` on Linux and `plugin.dll` on Windows from the same `mods/<name>/` folder, so one mod
folder that ships both files installs on either platform. The example Makefiles carry both
recipes: `make` for Linux, `make CROSS=mingw` for Windows.

One Windows rule: the engine and each plugin link separate C runtimes, so a memory block must be
freed by the module that allocated it — don't `free()` what the engine handed you, and don't
expect the engine to free what you allocated. (Allocating and freeing within your own plugin —
the normal case — is always fine, and the engine API already follows this rule.)

Developing on a Windows PC? Run the same builds inside WSL — a full Linux environment where the
recipes above apply unchanged, and the built `plugin.dll` runs directly on the same machine.

The five examples are the templates, one per capability: `hello` (data + lifecycle), `keybinds`
(scancode seam + game_ram), `item_grabber` (the full overlay toolkit), `wraptest` (a wrap with
`roth_next`), `doc_viewer` (a full replacement with passthrough).

## The honesty section

A plugin is **arbitrary native code running inside the game's process**. The `api_use` flags are
*declared API use* — a compatibility/diagnostic statement the loader can sanity-check, **not a
sandbox**. Nothing prevents a native plugin from doing anything the process can do. Install mods
you trust, exactly as you would for any native-modded game (Skyrim/SKSE, BepInEx…).

## Not currently supported

Per-function source fingerprints and changed-function release manifests (a "mod outdated" early
warning), zip-packaged mod archives, a scripting (Lua) tier, and `on_audio` dispatch are not
present. The SDK's append-only versioning is designed to accommodate each as ordinary additive
growth.
