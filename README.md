# ROTH.C

**A native recreation of `ROTH.EXE`, the engine of _Realms of the Haunting_ (1996), as verified,
readable C, with a built-in modding platform.**

_Realms of the Haunting_ shipped as a DOS program. ROTH.C is that program's engine, reconstructed
function by function into C that has been verified against the original executable's behavior, then
wrapped in a native host (SDL3) so it runs as an ordinary desktop application: windowed, with
working mouse capture, music, and sound. No DOSBox required!

The game's content is not included; you simply take a couple files from this project and drop it
into your existing Realms of the Haunting installation folder and you can play.

## Playing

1. Download the release bundle: `roth` (the engine), `libSDL3.so.0`, `gm.sf2` (soundfont), and an
   empty `mods/` folder.
2. Copy everything into your installation's `ROTH` directory. (this is the one containing `CONFIG.INI` and
   the game's data files, which is `"Realms of the Haunting/ROTH/"` in the Steam version).
3. Run `./roth` (or double-click it). The game starts windowed with music and sound.

**Note on MIDI:** Since the original game uses MIDI music and relied on a physical sound card back in the day, this native
version of the game relies on using a soundfont to determine how to play the MIDI tracks in the game.
This is the reason why `gm.sf2` needs to be copied over as well. This also means that you're free
to use your own soundfont without needing specialized MIDI-playing software.

## System

Currently, only Linux x86 is supported, but I am working on the Windows version next. The engine is
a 32-bit program by nature, which is kept preserverd, so 32-bit runtime libraries are required
(the release bundles its own SDL3).

## Mods

> First-party mods can be found here: [`roth-mods`](https://github.com/beyond-the-ire/roth-mods)

ROTH.C ships with built-in modding infrastructure, allowing mods to be added very easily. Simply create a `mods/` folder
in the installation directory (see step 2 under the Playing section above) and drop the mods you want into it.

Mods themselves are just folders with a few files in them. See [Modding](#modding) for more information.

## Building from source

Prerequisites: `gcc` with 32-bit multilib support, GNU `make`, `python3`, binutils, and the 32-bit
SDL3 build (fetches a pinned SDL release; needs `cmake`, `git`, network access,
and your distribution's 32-bit X11/ALSA/PipeWire/Mesa client libraries):

```
tools/build_sdl3_i386.sh        # one-time: builds roth_c/third_party/sdl3-i386
make -C sdk check               # the SDK's consistency gates (should print ALL GATES PASS)
make -C roth_c                  # the moddable engine + the dev viewer
make -C roth_c dist             # assemble the release bundle into roth_c/dist/
```

See [`roth_c/README.md`](roth_c/README.md) for build details, run options, and the development
tools.

### The two build flavors

- **moddable** (the default, what releases ship): carries the runtime plugin platform — a loader
  for mod plugins and inert padding at every engine function that lets mods override engine
  behavior at startup. With no mods installed, the engine verifies at boot that every pad is
  untouched.
- **vanilla** (`make -C roth_c FLAVOR=vanilla`): no mod surface at all — no loader, no padding.
  Mods are impossible by construction. The reference artifact for anyone who wants the
  reconstruction and nothing else.

## Modding

ROTH.C is built to be modded. Mods are native plugins loaded at startup from `mods/<name>/` —
users install them by extracting a folder, and remove them by deleting it. A mod can patch game
data at boot, remap keys, watch and rewrite input, draw overlays on every frame, call engine
functions, and wrap or replace individual engine functions. The engine's reconstructed source
makes "copy the real function, modify it, override it" a first-class technique.

Useful switches: `--no-mods` (start with the mod platform inert), `--list-mods` (show what would load and
exit), `--scale N` (initial window scale). The full list is in
[`roth_c/README.md`](roth_c/README.md).

- **Writing mods**: start with [`sdk/MODDING_GUIDE.md`](sdk/MODDING_GUIDE.md) — the mod types,
  the execution-context rules, the override chain semantics, and a build recipe. The SDK's
  machinery (schemas, generated headers, ABI gates) is documented in
  [`sdk/README.md`](sdk/README.md). Two minimal teaching examples live in `sdk/examples/`.
- **Ready-made mods**: the companion repository
  [`roth-mods`](https://github.com/beyond-the-ire/roth-mods) hosts downloadable mods (key
  rebinding, an item browser, an enlarged document viewer, …), each a drop-in folder.

## Repository layout

| path                   | contents                                                                                                     |
| ---------------------- | ------------------------------------------------------------------------------------------------------------ |
| `roth_c/src/engine/`   | the reconstructed game engine (see the note below)                                                           |
| `roth_c/src/data/`     | the engine's reconstructed static data, as named, typed C                                                    |
| `roth_c/src/platform/` | the native host: video/input/audio (SDL3), the DOS-era OS services the engine expects, the mod plugin loader |
| `sdk/`                 | the modding SDK: schemas, generated headers, ABI baselines, the guide, teaching examples                     |
| `tools/`               | the SDK generators/validators and the SDL3 build script                                                      |
| `docs/reference/`      | machine-readable reference data the SDK tooling consumes                                                     |

## Fidelity — the project's one law

`roth_c/src/engine/` is a **preservation artifact**. It reproduces the original engine's behavior (+ its quirks)
with every function verified against the original executable, at the
level of instructions and bytes, before it was accepted. It is deliberately not "improved":
faithful reconstruction is the point, and it is what makes the source trustworthy as
documentation of how the game actually works.

Consequences worth knowing:

- Bugs that existed in the original are preserved (unless they crash; a handful of provable
  original niche defects are guarded in the moddable build only, each documented at the site with the
  original's disassembly).
- Enhancements do not go into the engine. They belong in either the platform layer or as mods.
- Changes to the engine source are accepted only as _fidelity fixes_: corrections that bring the
  C closer to the original's actual behavior, with evidence. See
  [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Credits

- **_Realms of the Haunting_** was developed by **Gremlin Interactive** (1996). This project is a
  fan reconstruction and is not affiliated with, endorsed by, or connected to the game's
  developers, publishers, or rights holders. All trademarks belong to their owners.
- The bundled soundfont is **GeneralUser GS** by S. Christian Collins.
- Built with **SDL 3** and **TinySoundFont**.

## License

Two-part (see [`LICENSE`](LICENSE) for the full text):

- **The reconstructed engine** (`roth_c/src/engine/`, `roth_c/src/data/`, `docs/reference/`) is
  published as a preservation notice: provided for preservation, study, and interoperability;
  the underlying game's rights remain entirely with its rights holders, and rights-holder
  concerns will be addressed promptly and in good faith.
- **The original work** (the platform layer, the SDK, the tooling, the documentation) is
  **MIT-licensed**, meaning mods built against the SDK may be distributed freely.

No game assets are included in this repository or its releases; the original game files must be
obtained from a lawfully acquired copy of the game, such as from [GOG](https://www.gog.com/en/game/realms_of_the_haunting)
or [Steam](https://store.steampowered.com/app/292390/Realms_of_the_Haunting/).
