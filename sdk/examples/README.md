# ROTH.C SDK examples

Five example plugins, one per capability. Each is a self-contained template: it compiles against
**only** `sdk/include/roth_sdk.h` (no engine headers, no repo internals), so copying a directory
elsewhere and pointing its `Makefile` at the SDK header is all it takes to start a new mod. Read
[`../MODDING_GUIDE.md`](../MODDING_GUIDE.md) first for the concepts these demonstrate.

| example | demonstrates |
|---|---|
| `hello` | the minimal plugin: the lifecycle callbacks (`on_load` → `on_unload`), reading `game_ram`, and drawing into the compose-tick frame — also the self-containment proof |
| `keybinds` | a **data mod** plus the **scancode input seam**: rebinds the engine's keybind tables in `game_ram` at boot and rewrites menu-navigation keys through `on_scancode` |
| `item_grabber` | the full **overlay toolkit**: the compose-tick, mouse-poll, and scancode seams driving a custom UI, with exposed engine calls behind it |
| `wraptest` | a function **wrap** via the override registry — decorates a function and forwards through `roth_next` |
| `doc_viewer` | a full function **replacement** with clean passthrough — replaces a function's body, or returns `roth_next` when disabled |

## Building

Each example builds with a plain `make` in its own directory:

```
cd hello
make          # -> plugin.so
```

**Requirements:** `gcc` with 32-bit support (`-m32`; on Debian/Ubuntu that is the
`gcc-multilib` package). The recipe every example shares:

```
gcc -m32 -shared -fPIC -std=c11 -Wall -Wextra -Werror -I<sdk>/include -o plugin.so <name>.c
```

## Installing a built plugin

A plugin is a folder next to the game, named for the mod, containing the compiled `plugin.so` and
any of the mod's own files (config, assets):

```
<game>/mods/<name>/plugin.so        # e.g. mods/hello/plugin.so
<game>/mods/<name>/<name>.cfg        # if the example ships one, copy it too
```

`plugin.so` is the only required file. For the examples that ship a `.cfg`, copy the whole
directory's contents so the config lands beside the `.so`.

## Verifying it loads

Run the moddable binary with `--list-mods`: it prints every plugin that would load (id, name,
version, ABI) and exits without starting the game. If your plugin appears in that list, the host
found it, its ABI matched, and it passed validation.

```
roth --list-mods
```

Related flags: `--no-mods` (or `ROTH_MODS=0`) boots with the platform inert, `--strict-mods` turns
validation warnings into hard aborts, and `--dump-mod-chains` prints the resolved override chains.
