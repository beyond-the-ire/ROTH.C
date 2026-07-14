# Contributing to ROTH.C

Thanks for your interest! This repository has a somewhat unusual shape: part of it is a **preservation
artifact** with a strict fidelity rule, and part of it is ordinary software that welcomes ordinary
contributions. Please read this page before opening a pull request.

If you find any inconsistencies in comments, function names, or documentation, feel free to open a PR.
This is still an evolving project.

## The two zones

### 1. The engine (`roth_c/src/engine/`, `roth_c/src/data/`)

This code is a verified reconstruction of the original 1996 executable. Its value comes from
being _trustworthy_: every function was checked against the original program's actual behavior
before acceptance, and the source doubles as documentation of how the game truly works.

For that reason, things such as refactors, tweaks, modernization, performance work, etc
are not acceptable to be changed on the engine level. The code is intentionally shaped
as similar to the original as possible and should therefore stay preserved as such. You
might instead want to consider creating a mod for your tweak or change (see `sdk/MODDING_GUIDE.md`)

On the other hand, the engine should accept changes where the reconstructed codebase
demonstrably differs from the original engine's behavior. This must be backed up by
evidence, such as disassembly, screenshots, etc.

If you've found a divergence but can't make the low-level case yourself, **open an issue** with
your observations; a well-documented symptom is valuable on its own.

The engine also naturally accepts any corrections or clarifications relating to comments,
documentation, naming, and other things of that nature.

One narrow exception exists in the engine files: crash guards for provable _original_ defects
(the original program would fault identically), compiled only into the moddable build and each
documented in place with the original's disassembly. New guards of this kind are held to the
same evidence standard.

### 2. Everything else: fair game

The platform layer (`roth_c/src/platform/`), the SDK and its tooling (`sdk/`, `tools/`), the
build system, and the documentation are regular software. Bug fixes, portability work, build
improvements, and documentation fixes are all welcome as ordinary pull requests.

Current areas where help is genuinely useful: platform portability, packaging for distributions,
and documentation clarity for mod authors.

## Mods

Mods do not live in this repository. Publish your mod anywhere you like (it's a folder with a
`plugin.so` and its files), or look at the companion
[`roth-mods`](https://github.com/beyond-the-ire/roth-mods) repository. Additions to the **SDK
surface** (exposing more engine functions, new callbacks) can be proposed here — note that the
SDK grows strictly append-only, and the CI gates in `sdk/` enforce the compatibility rules
mechanically.

## Pull-request checklist

- [ ] Both types build: `make -C roth_c` and `make -C roth_c FLAVOR=vanilla`.
- [ ] The SDK gates pass: `make -C sdk check` prints `ALL GATES PASS`.
- [ ] No diffs under `roth_c/src/engine/` or `roth_c/src/data/` — unless the PR _is_ a fidelity
      fix, in which case the evidence comes with it.
- [ ] The PR description says what was tested and how.
- [ ] No game assets, no fragments of the original executable, and no copyrighted game content
      in the diff, the issue, or the attachments. This is a hard rule with no exceptions.

## Bug reports

Two useful flavors:

- **Engine-behavior reports** — "the game does X here, and the original does Y": name the spot
  (map/room, what you were doing), attach screenshots, and if you can, the same scene from the
  original game for comparison. A save file positioned at the spot makes a report actionable
  almost by itself.
- **Platform reports** — crashes, build failures, audio/video issues: include your distribution,
  how you launched the game, and the terminal output. For crashes, a captured core dump's stack
  trace is helpful.
