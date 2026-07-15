# roth_c/Makefile — the PRODUCT build. Compiles the standalone binary that REPLACES ROTH.EXE
# entirely from THIS tree — src/engine (the verified game engine), src/data (engine data as
# named/typed C), src/platform (the Linux/POSIX host layer).
#
#   make                 -> roth  (the moddable game binary)
#   make roth
#   make viewer          -> the optional headless-mode dev viewer (roth-view; needs system SDL2)
#
# The moddable game binary is named `roth` (the vanilla/reference flavor is `roth-vanilla`). It maps
# NO original ROTH.EXE bytes: no original-image bridge (no call_orig, no in-place patcher, no trap
# servicer) — the arena + host video/DOS memory + obj3_build's reconstructed data are all it needs.
# (A separate differential trap host — a development-only instrument used to validate this engine
# against the original — is built elsewhere and is NOT part of this tree.)
# Compatibility aliases `roth-host-imgfree` / `roth-host-vanilla` still build the current names (below).
# --- build axis: CROSS (empty = native Linux i386 ; 'mingw' = i686 Windows PE) ------------------
# CROSS unset leaves every native line byte-identical (the else branch below == the pre-axis
# values). CROSS=mingw swaps CC to the MinGW i686 cross-gcc, drops the ELF-isms (-m32/-no-pie/
# -lrt), adds the PE placement flags (rebase high above the 0x10000..0x4a9000 arena + ASLR off;
# NOT --disable-high-entropy-va — that is 64-bit-only, i686 ld rejects it and the link fails),
# names the product rothc.exe (never roth.exe: on a case-insensitive FS it would clobber the original ROTH.EXE),
# and links the win32 SDL import lib. Four never-shared object trees: build/, build-vanilla/,
# build-mingw/, build-mingw-vanilla/.
# rothc.exe is a windows-subsystem binary (no console window on double-click).
# (Trailing whitespace before an inline comment is captured by := — keep such comments on their
#  own line for any value that composes a filename, e.g. BINBASE below.)
WIN_SUBSYSTEM ?= windows
CROSS ?=
ifeq ($(CROSS),mingw)
  XPFX     := i686-w64-mingw32-
  CC       := $(XPFX)gcc
  EXE      := .exe
  # NEVER roth.exe — on a case-insensitive FS beside ROTH.EXE it would clobber the 1996 binary
  BINBASE  := rothc
  BUILDTAG := -mingw
  # 32-bit is native to this toolchain: NO -m32. PE relocates via base relocs, so -fno-pie/-no-pie
  # are ELF-only and dropped. _GNU_SOURCE kept for parity (harmless on mingw; feature-test only).
  # -include src/platform/mingw_compat.h: force a compile prelude ahead of every unit on this axis.
  # It renames the engine's own stricmp (int32_t(const uint8_t*,const uint8_t*)) away from the
  # MinGW CRT's deprecated stricmp oldname (int(const char*,const char*)); glibc has no such alias,
  # so the native build never needs it. See that header for the include-ordering rationale.
  CFLAGS   := -O1 -g -Wall -Wextra -D_GNU_SOURCE -fno-stack-protector -include src/platform/mingw_compat.h
  LDFLAGS  := -Wl,--image-base,0x10000000 -Wl,--disable-dynamicbase -m$(WIN_SUBSYSTEM)
  SDL3_DIR := third_party/sdl3-win32
  NM       := $(XPFX)nm
  OBJDUMP  := $(XPFX)objdump
  STRIP    := $(XPFX)strip
else
  XPFX     :=
  CC       := gcc
  EXE      :=
  BINBASE  := roth
  BUILDTAG :=
  CFLAGS   := -m32 -O1 -g -Wall -Wextra -D_GNU_SOURCE -fno-pie -fno-stack-protector
  LDFLAGS  := -m32 -no-pie -lrt
  SDL3_DIR := third_party/sdl3-i386
  NM       := nm
  OBJDUMP  := objdump
  STRIP    := strip
endif

ENGINE   := src/engine
PLATFORM := src/platform
DATA     := src/data
SDK_DIR  := ../sdk
SDK_INC  := $(SDK_DIR)/include
CFLAGS   += -I$(ENGINE) -I$(PLATFORM) -I$(DATA)

# --- build flavor -------------------------------------------------------------------------------
# Compile-time mods are ABOLISHED: there is NO compile-time override mechanism and no mod code in
# either shipped artifact. ALL mods are runtime plugins. The ONLY build-time question is the flavor:
#
#   make  /  make FLAVOR=moddable   (DEFAULT — the shipped product): engine objects carry the
#                     -fpatchable-function-entry=5,0 NOP pads; the plugin loader + override registry are
#                     linked; the plugin dispatch call sites are compiled in. Output: roth.
#                     `make dist` ships this. Not loading any plugin = pads stay NOP = pristine.
#   make FLAVOR=vanilla   the preservation/reference artifact: NO pads (no -fpatchable-function-entry),
#                     NO loader/registry (PLUGIN_OBJS/OVERRIDE_OBJS unlinked), the plugin dispatch call
#                     sites compiled OUT (-DROTH_VANILLA). Mods are impossible by construction. Output:
#                     roth-vanilla.
#
# The two flavors NEVER share objects: vanilla builds into build-vanilla/ (its engine objects differ —
# no pad flag — and its host objects differ — -DROTH_VANILLA), so a flavor switch can never pick up a
# stale object from the other. Both keep the nm-clean gate.
FLAVOR ?= moddable
ifeq ($(FLAVOR),vanilla)
  BUILD       := build$(BUILDTAG)-vanilla
  PRODUCT_BIN := $(BINBASE)-vanilla$(EXE)
  CFLAGS      += -DROTH_VANILLA
else ifeq ($(FLAVOR),moddable)
  BUILD       := build$(BUILDTAG)
  PRODUCT_BIN := $(BINBASE)$(EXE)
  ifneq ($(CROSS),mingw)
    LDFLAGS   += -ldl               # the plugin loader's dlopen/dlsym (glibc>=2.34 folds into libc; explicit for -m32). dlopen is in the win32 loader (kernel32) — drop -ldl on mingw.
  endif
else
  $(error FLAVOR must be 'moddable' or 'vanilla' (got '$(FLAVOR)'))
endif

# --- 32-bit SDL3 for the in-process window/audio ------------------------------------------------
# tools/build_sdl3_i386.sh builds an i386 SDL3 into third_party/sdl3-i386/ (git-ignored).
# It is wired into the roth game binary ONLY (never the --headless path): the SDL TU
# (src/platform/sdl/sdl_host.c) compiles with -I$(SDL3_INC) + -DROTH_STANDALONE, and the
# standalone link adds $(SDL_LDLIBS). Dev builds rpath at this install's lib/; the release ships a
# $ORIGIN-rpath'd libSDL3.so.0 beside the exe. SDL symbols are SDL_* so the nm-clean
# assertion below is unaffected.
# SDL3_DIR is set per-axis in the CROSS block above (sdl3-i386 native / sdl3-win32 mingw).
SDL3_INC := $(SDL3_DIR)/include
SDL3_LIB := $(SDL3_DIR)/lib
# the in-process SDL3 window + audio consumer TUs + link libs (standalone only; -pthread for
# the game thread; -lm for the TinySoundFont synth's float DSP in sdl_tsf.o).
SDL_OBJS   := $(BUILD)/sdl_host.o $(BUILD)/sdl_audio.o $(BUILD)/sdl_tsf.o
ifeq ($(CROSS),mingw)
# WINDOWS: -lSDL3 resolves the import lib libSDL3.dll.a; SDL3.dll is found beside rothc.exe at
# runtime (the Windows loader default — no rpath/$ORIGIN analog). No -rpath (PE has none).
# The MinGW support runtime is linked STATICALLY so the shipped folder is self-contained: the only
# non-system DLL beside the exe is SDL3.dll. -static-libgcc folds libgcc into the image; the
# -Bstatic..-Bdynamic pair links winpthreads (game_main is a pthread) INTO the exe instead of
# depending on libwinpthread-1.dll, while leaving the system import libraries (kernel32, the UCRT)
# dynamic. A blanket -static is deliberately NOT used: it would also force a static SDL3, which then
# fails to link on unresolved COM/mmdevice imports.
SDL_LDLIBS := -L$(SDL3_LIB) -lSDL3 -lm -static-libgcc -Wl,-Bstatic,-lwinpthread,-Bdynamic
else
# LINUX DUAL rpath: '$ORIGIN' is searched FIRST so a libSDL3.so.0 bundled beside the binary (the
# `make dist` product folder) wins; in-tree that dir has no SDL, so the loader falls through to the
# absolute third_party build path. The SAME roth binary therefore runs both in-tree and in dist/.
# ('$$ORIGIN' -> make emits the literal $ORIGIN; the single quotes stop the shell expanding it.)
SDL_LDLIBS := -pthread -L$(SDL3_LIB) -lSDL3 -lm \
              -Wl,-rpath,'$$ORIGIN' -Wl,-rpath,$(abspath $(SDL3_LIB))
endif

# --- Windows application icon + version resource (CROSS=mingw only, optional) --------------------
# rothc.exe embeds an application icon (and version info) when an icon file is present. The artwork
# is a DROP-IN: place a Windows .ico at res/rothc.ico and the next `make CROSS=mingw ...` compiles it
# into the executable automatically (Explorer and the taskbar show the first icon in the image). With
# no icon file the build succeeds and links no resource — the executable is simply icon-less until
# the art lands. windres is the PE resource compiler, so this is only ever active on the CROSS=mingw
# axis; the native Linux build never references it and is unchanged.
WIN_ICON    := res/rothc.ico
WINDRES     := $(XPFX)windres
WIN_RES_OBJ :=
ifeq ($(CROSS),mingw)
  ifneq ($(wildcard $(WIN_ICON)),)
    WIN_RES_OBJ := $(BUILD)/rothc_res.o
  endif
endif

# flag matrix:
#   STANDALONE_CFLAGS  the verified engine TUs + traps (absolute/pinned-arena addressing; -DROTH_STANDALONE
#                   + -ffunction-sections/--gc-sections so the unreferenced original-image bridge + int-trap
#                   servicer garbage-collect away)
#   MAINIF_CFLAGS   main.c compiled -DROTH_STANDALONE (main_if.o = the image-free program entry; NOT sectioned)
#   DATAC_CFLAGS    the datac data-as-C TUs (sectioned, no -DROTH_STANDALONE)
#   plain CFLAGS    the always-native host services (dos/dpmi/audio/os_api/…) + boot
STANDALONE_CFLAGS := $(CFLAGS) -DROTH_STANDALONE -ffunction-sections -fdata-sections
MAINIF_CFLAGS     := $(CFLAGS) -DROTH_STANDALONE
DATAC_CFLAGS      := $(CFLAGS) -ffunction-sections -fdata-sections

# --- override-registry pad ----------------------------------------------------------------------
# MODDABLE flavor: the ENGINE objects get -fpatchable-function-entry=5,0: five NOP bytes AFTER
# each function entry (none before). The boot-time override registry overwrites those five bytes in
# place with a jmp rel32 to a per-function dispatch thunk; call-next re-enters the recorded body at
# entry+5. VANILLA flavor: NO pad flag — the entries are ordinary, mods impossible by construction.
# SCOPED TO ENGINE TUs ONLY: this flag rides the generic $(BUILD)/%_if.o rule below, which matches
# *only* src/engine/<stem>.c (traps_if/main_if/sdl_host/sdl_audio/datac all have their OWN explicit
# rules and keep plain STANDALONE_CFLAGS — verified: none of them backs the generic pattern rule).
# So traps/host/SDL objects get NO pad. (A separate development-only differential host compiles the
# same engine sources with its own flags into its own objects and is wholly unaffected.)
# +~2% .text (measured). Pad geometry is not plugin-visible ABI.
ifeq ($(FLAVOR),vanilla)
ENGINE_STANDALONE_CFLAGS := $(STANDALONE_CFLAGS)
else
ENGINE_STANDALONE_CFLAGS := $(STANDALONE_CFLAGS) -fpatchable-function-entry=5,0
endif

LIFT_TUS := renderer raw_commands math_util memory_pool collision_physics text_font automap doors savegame player dbase100 gdv_cutscene weapon_combat inventory game_core audio das_assets input blit_2d video_display render_world menu_hud_ui map_load dialogue_ui file_config dos_runtime entity_ai dpmi_dos_os
STANDALONE_LIFT_OBJS := $(addprefix $(BUILD)/,$(addsuffix _if.o,$(LIFT_TUS)))

# the always-native host services (compiled plain here for the product)
PLATFORM_OBJS := $(BUILD)/dos.o $(BUILD)/dpmi.o $(BUILD)/audio.o $(BUILD)/audio_trace.o \
                 $(BUILD)/os_api.o $(BUILD)/standalone_hooks.o $(BUILD)/os_audio.o \
                 $(BUILD)/os_audio_standalone.o
# image-free-only host TUs
STANDALONE_ONLY_OBJS := $(BUILD)/traps_if.o $(BUILD)/main_if.o $(BUILD)/boot.o
# the per-OS system seam (the tick/timer + dl-compat + protect-exec + low-alloc + exe-dir + dir-enum,
# plus the fixed-mapping arena and the software selector ledger). The POSIX bodies live in
# sys/sys_posix.c; the Windows bodies in sys/sys_win32.c. Each axis links exactly one seam object.
ifeq ($(CROSS),mingw)
  PLATFORM_OS_OBJS := $(BUILD)/sys_win32.o
else
  PLATFORM_OS_OBJS := $(BUILD)/sys_posix.o
endif
# datac data-as-C the image-free link consumes (obj3_build overlays obj3_owned + reloc pass; obj1 arena)
DATAC_OBJS := $(BUILD)/obj3_build_if.o $(BUILD)/obj3_data_if.o $(BUILD)/obj3_owned_if.o \
              $(BUILD)/obj1data_if.o $(BUILD)/obj1_owned_if.o
STANDALONE_HOST_OBJS := $(PLATFORM_OBJS) $(PLATFORM_OS_OBJS) $(STANDALONE_ONLY_OBJS) $(DATAC_OBJS)
# There is no compile-time mod mechanism: mods are never selected or built at compile time. ALL mods
# are runtime plugins (sdk/examples/), loaded by the platform below in the moddable flavor.

# the runtime PLUGIN platform — standalone-only by linkage.
# plugin_loader.o = the dlopen/validate/lifecycle state machine; host_adapters.o = the GENERATED
# host API table (sdk/generated/host_adapters.c) filled against the real engine + game_ram.
PLUGIN_OBJS := $(BUILD)/plugin_loader.o $(BUILD)/host_adapters.o

# the boot-time OVERRIDE REGISTRY — standalone only.
# override_registry.o = the chain builder + boot-time entry patcher; override_gen.o = the GENERATED
# per-eligible-function dispatch thunks + target table (build/override_gen.c, from override_targets.json).
PY        := python3
TOOLS     := ../tools
OVERRIDE_OBJS := $(BUILD)/override_registry.o $(BUILD)/override_gen.o

# The mod objects the LINK consumes, by flavor: moddable links the loader + override registry; vanilla
# links neither (mods impossible by construction). The engine lift objects are the SAME set for both
# flavors (no compile-time override mechanism) — they differ only in the pad flag,
# which is handled per-flavor in ENGINE_STANDALONE_CFLAGS above (into a separate build dir).
ifeq ($(FLAVOR),vanilla)
MOD_LINK_OBJS :=
else
MOD_LINK_OBJS := $(PLUGIN_OBJS) $(OVERRIDE_OBJS)
endif

.PHONY: all viewer clean dist dist-win _dist-win-guard roth-host-imgfree roth-host-vanilla
all: $(PRODUCT_BIN)

# compatibility aliases (old internal artifact names) — keep every pre-rename invocation working.
# `make roth-host-imgfree` builds the moddable `roth`; `make FLAVOR=vanilla roth-host-vanilla` builds
# `roth-vanilla`. Each is a no-op when its target is already up to date.
roth-host-imgfree: roth                 # compatibility alias -> the moddable game binary `roth`
roth-host-vanilla: roth-vanilla         # compatibility alias -> the vanilla flavor `roth-vanilla`

# NOTE: the differential trap host links capture.o/calltrace.o (the ROTH_TRACE/ROTH_CAPTURE
# instrument), but they are dead code in the standalone build — calltrace_init/capture_init are in
# main.c's trap-only #else branch and the dispatch calls live in the gc'd int-trap servicer. They are
# therefore NOT linked here. (Proven: nm on the result carries no calltrace_/capture_.)
$(PRODUCT_BIN): $(STANDALONE_HOST_OBJS) $(STANDALONE_LIFT_OBJS) $(MOD_LINK_OBJS) $(SDL_OBJS) $(WIN_RES_OBJ)
	$(CC) $(LDFLAGS) -Wl,--gc-sections -o $@ $^ $(SDL_LDLIBS)
ifeq ($(CROSS),mingw)
	@echo "== nm-clean assertion (PE, underscore-aware): no original-bytes bridge (call_orig / lift_install / trampoline / int3 resume) =="
	@# i686-PE prefixes C symbols with a leading underscore, so the native gate's raw grep would
	@# silently miss _call_orig (a false PASS). Strip that decoration, then apply the SAME forbidden
	@# list to defined globals only. Unlike the ELF linker, the PE linker's --gc-sections does not
	@# drop a small set of unreferenced, fail-closed veneers (call_orig/call_orig_raw/call_orig_far
	@# and the two NULL int3 suspend/resume function pointers): on the image-free path they bridge to
	@# nothing (the far veneer aborts if ever entered), and the native gate over the identical sources
	@# already proves nothing reaches them. They are enumerated and excused here; ANY other bridge
	@# symbol — an install/live-swap trampoline, or a newly introduced one — survives the filter and
	@# fails the gate.
	@if $(NM) $@ | sed -E 's/^([0-9a-fA-F]+ [A-Za-z] )_/\1/' | awk '$$2 ~ /^[A-Z]$$/ {print $$3}' \
	     | grep -wE 'call_orig|call_orig_raw|call_orig_far|lift_install|lift_resume_int3s|lift_suspend_int3s|g_os_resume_int3s|g_os_suspend_int3s|trampoline' \
	     | grep -vxE 'call_orig|call_orig_raw|call_orig_far|g_os_resume_int3s|g_os_suspend_int3s' \
	     | grep -v '^sos_user_callback_trampoline$$' | grep . ; then \
	   echo "FAIL: image-free binary retains an original-bytes bridge symbol"; exit 1; \
	 else echo "PASS: $(PRODUCT_BIN) is call_orig/lift_install/trampoline-free (bar the enumerated inert veneers)"; fi
  ifeq ($(FLAVOR),moddable)
	@echo "== override-registry pad validation (PE, moddable flavor) =="
	@$(PY) $(TOOLS)/sdk_override_validate.py --pe --objdump $(OBJDUMP) $@ || (echo "FAIL: PE override pad validation"; exit 1)
  else
	@echo "== vanilla flavor: NO override pads (PE; mods impossible by construction) =="
	@$(PY) $(TOOLS)/sdk_override_validate.py --pe --expect-no-pads --objdump $(OBJDUMP) $@ || (echo "FAIL: PE vanilla pad leak"; exit 1)
  endif
else
	@echo "== nm-clean assertion (no original-bytes bridge: call_orig / lift_install / trampoline / int3 resume) =="
	@if nm $@ | grep -wE 'call_orig|call_orig_raw|call_orig_far|lift_install|lift_resume_int3s|lift_suspend_int3s|g_os_resume_int3s|g_os_suspend_int3s|trampoline' | grep -v sos_user_callback_trampoline; then \
	   echo "FAIL: image-free binary retains an original-bytes bridge symbol"; exit 1; \
	 else echo "PASS: $(PRODUCT_BIN) is call_orig/lift_install/trampoline-free"; fi
  ifeq ($(FLAVOR),moddable)
	@echo "== override-registry pad validation (moddable flavor) =="
	@$(PY) $(TOOLS)/sdk_override_validate.py $@ || (echo "FAIL: override pad validation"; exit 1)
  else
	@echo "== vanilla flavor: NO override pads (mods impossible by construction) =="
	@# readelf -SW (WIDE) so the long section name is not truncated (a plain -S clips it -> false pass).
	@if readelf -SW $@ 2>/dev/null | grep -q __patchable_function_entries; then \
	   echo "FAIL: vanilla binary unexpectedly has __patchable_function_entries"; exit 1; \
	 else echo "PASS: $(PRODUCT_BIN) has no __patchable_function_entries section"; fi
  endif
endif

viewer:
	$(MAKE) -C $(PLATFORM)/viewer all

# --- make dist: the shippable, self-contained product folder ------------------------------------
# Layout (dist/, git-ignored):
#   roth            the game binary (a stripped copy of the moddable roth binary) — resolves libSDL3 via the
#                   '$ORIGIN' rpath baked into it, so the bundled lib beside it is found first
#   libSDL3.so.0    a STRIPPED copy of the bundled i386 SDL3 (SONAME libSDL3.so.0)
#   gm.sf2          the General MIDI SoundFont (GeneralUser GS) — replaceable (see README.txt)
#   README.txt      install + usage note
# Install: copy the dist/ contents next to the ROTH game files (CONFIG.INI, ROTH.RES, DBASE*.DAT,
# *.DAS) and run ./roth — with no args it uses the exe's own directory as the game dir.
DIST := dist
define DIST_README
Realms of the Haunting — native host (image-free recompilation)
==============================================================

A single self-contained binary that replaces ROTH.EXE and plays the game from the
original content files (ROTH.RES, DBASE*.DAT, *.DAS, CONFIG.INI). No DOSBox, no
original executable bytes.

INSTALL
  1. Copy everything in this folder (roth, libSDL3.so.0, gm.sf2, README.txt) into
     your ROTH game directory — the folder that contains CONFIG.INI and ROTH.RES.
  2. Run:  ./roth
     With no arguments it uses its own directory as the game directory and opens a
     window with video, input and audio. Close the window (or press the usual quit)
     to exit.

USEFUL OPTIONS
  ./roth --game-dir DIR   play content in DIR instead of the exe's own folder
  ./roth --headless       no window; publish the shared framebuffer (dev/CI/test rigs)
  ./roth --scale N        initial window scale (also ROTH_SCALE=N)
  ROTH_SF2=/path/to.sf2   use a different SoundFont for the MIDI music (see below)
  ROTH_MIDI=0             disable the MIDI music (digital SFX/speech still play)

SOUNDFONT (MIDI music)
  The bundled gm.sf2 is "GeneralUser GS 2.0.3" by S. Christian Collins
  (GeneralUser GS License v2.0, which permits redistribution). You may replace it
  with any General MIDI .sf2: either overwrite gm.sf2 in this folder, or point
  ROTH_SF2 at your own file. If no SoundFont is found the game runs without music.

libSDL3.so.0
  The bundled 32-bit SDL3 is found automatically (it sits beside the binary). It is
  unmodified upstream SDL3 (zlib license).
endef
export DIST_README

dist: roth
	rm -rf $(DIST)
	mkdir -p $(DIST)
	cp roth $(DIST)/roth
	strip $(DIST)/roth
	cp $(realpath $(SDL3_LIB)/libSDL3.so.0) $(DIST)/libSDL3.so.0
	chmod u+w $(DIST)/libSDL3.so.0
	strip $(DIST)/libSDL3.so.0
	cp $(PLATFORM)/viewer/gm.sf2 $(DIST)/gm.sf2
	printf '%s\n' "$$DIST_README" > $(DIST)/README.txt
	mkdir -p $(DIST)/mods   # drop plugin folders (mods/<name>/plugin.so) here at runtime
	@echo "== dist/ built =="; ls -la $(DIST)
	@echo "== dist/ ldd (libSDL3 must resolve to the bundled \$$ORIGIN copy) =="; \
	 (cd $(DIST) && ldd ./roth | grep -i sdl3) || true

# --- make CROSS=mingw dist-win: the shippable, self-contained Windows folder --------------------
# Layout (dist-win/, git-ignored) — extract these beside the game files (CONFIG.INI, ROTH.RES,
# DBASE*.DAT, *.DAS) and run rothc.exe:
#   rothc.exe      the game executable (a stripped copy of the moddable Windows binary). The MinGW
#                  support runtime is linked in statically, so no libwinpthread-1.dll / libgcc DLL
#                  is needed beside it.
#   SDL3.dll       the bundled Windows SDL3 (found automatically beside the exe by the loader)
#   gm.sf2         the General MIDI SoundFont (GeneralUser GS) — replaceable (see README.txt)
#   README.txt     install + usage note
#   mods/          empty; drop plugin folders (mods/<name>/plugin.dll) here at runtime
# Invoke on the CROSS axis: `make -C roth_c CROSS=mingw dist-win` (the guard below enforces it so
# PRODUCT_BIN, STRIP and the moddable flavor resolve to their Windows values).
DIST_WIN := dist-win
define DIST_README_WIN
Realms of the Haunting — native host (image-free recompilation) — Windows
=========================================================================

A single self-contained executable that plays the game from the original content
files (ROTH.RES, DBASE*.DAT, *.DAS, CONFIG.INI). No DOSBox, no original executable.

INSTALL
  1. Copy everything in this folder (rothc.exe, SDL3.dll, gm.sf2, README.txt) into
     your ROTH game directory — the folder that contains CONFIG.INI and ROTH.RES.
  2. Run:  rothc.exe
     With no arguments it uses its own directory as the game directory and opens a
     window with video, input and audio. Close the window (or press the usual quit)
     to exit. Saves are written to ROTH\SAVEGAME\ exactly like the original.

USEFUL OPTIONS
  rothc.exe --game-dir DIR   play content in DIR instead of the exe's own folder
  rothc.exe --scale N        initial window scale (also ROTH_SCALE=N)
  ROTH_SF2=C:\path\to.sf2    use a different SoundFont for the MIDI music (see below)
  ROTH_MIDI=0                disable the MIDI music (digital SFX/speech still play)

SOUNDFONT (MIDI music)
  The bundled gm.sf2 is "GeneralUser GS 2.0.3" by S. Christian Collins
  (GeneralUser GS License v2.0, which permits redistribution). You may replace it
  with any General MIDI .sf2: either overwrite gm.sf2 in this folder, or point
  ROTH_SF2 at your own file. If no SoundFont is found the game runs without music.

SDL3.dll
  The bundled SDL3 is found automatically (it sits beside the executable). It is
  unmodified upstream SDL3 (zlib license).
endef
export DIST_README_WIN

# a guard prerequisite so a stray `make dist-win` (no CROSS) fails cleanly BEFORE building anything,
# rather than composing a Linux binary into a Windows folder.
_dist-win-guard:
	@test "$(CROSS)" = "mingw" || { echo "dist-win requires CROSS=mingw (got CROSS='$(CROSS)')"; exit 1; }

dist-win: _dist-win-guard $(PRODUCT_BIN)
	rm -rf $(DIST_WIN)
	mkdir -p $(DIST_WIN)
	cp $(PRODUCT_BIN) $(DIST_WIN)/rothc.exe
	$(STRIP) $(DIST_WIN)/rothc.exe
	cp third_party/sdl3-win32/bin/SDL3.dll $(DIST_WIN)/SDL3.dll
	cp $(PLATFORM)/viewer/gm.sf2 $(DIST_WIN)/gm.sf2
	printf '%s\n' "$$DIST_README_WIN" > $(DIST_WIN)/README.txt
	mkdir -p $(DIST_WIN)/mods   # drop plugin folders (mods/<name>/plugin.dll) here at runtime
	@echo "== dist-win/ built =="; ls -la $(DIST_WIN)

ENGINE_HDRS := $(ENGINE)/engine.h $(ENGINE)/common.h $(ENGINE)/g_names.h

$(BUILD):
	mkdir -p $(BUILD)

# --- the verified engine TUs, image-free (absolute/pinned-arena addressing) ---
# ENGINE_STANDALONE_CFLAGS adds the override-registry pad. This pattern rule matches ONLY
# src/engine/<stem>.c — every non-engine *_if.o (traps/main/datac) has an explicit rule above/below.
$(BUILD)/%_if.o: $(ENGINE)/%.c $(ENGINE)/engine.h $(ENGINE)/common.h | $(BUILD)
	$(CC) $(ENGINE_STANDALONE_CFLAGS) -c -o $@ $<

# --- image-free-only host TUs ---
# shared traps.c, image-free (function-sectioned so the unreached int-trap servicer gc's away). The
# only seam consumer now is the plugin loader (plugins_dispatch_scancode/_compose_tick, guarded
# #ifdef ROTH_STANDALONE && !ROTH_VANILLA); there is no compile-time mods layer, so no -Imods.
$(BUILD)/traps_if.o: $(PLATFORM)/traps.c | $(BUILD)
	$(CC) $(STANDALONE_CFLAGS) -c -o $@ $<
# main.c compiled -DROTH_STANDALONE (the image-free program entry / boot dispatch). Includes the
# SDL-free sdl/sdl_host.h (no SDL headers needed here — only sdl_host.c pulls in SDL3).
$(BUILD)/main_if.o: $(PLATFORM)/main.c $(PLATFORM)/sdl/sdl_host.h | $(BUILD)
	$(CC) $(MAINIF_CFLAGS) -c -o $@ $<
# the in-process SDL3 window (native SDL3; ROTH_STANDALONE + the SDL3 include path). Linked ONLY
# into the roth game binary — the differential trap host never compiles or links this TU.
$(BUILD)/sdl_host.o: $(PLATFORM)/sdl/sdl_host.c $(PLATFORM)/sdl/sdl_host.h $(PLATFORM)/sdl/sdl_audio.h $(PLATFORM)/shared_fb.h | $(BUILD)
	$(CC) $(STANDALONE_CFLAGS) -I$(SDL3_INC) -c -o $@ $<
# the in-process SDL3 audio consumer (PCM ring drain + MIDI->TSF), native SDL3. Full warnings.
$(BUILD)/sdl_audio.o: $(PLATFORM)/sdl/sdl_audio.c $(PLATFORM)/sdl/sdl_audio.h $(PLATFORM)/viewer/tsf.h \
                      $(PLATFORM)/shared_audio.h $(PLATFORM)/shared_midi.h | $(BUILD)
	$(CC) $(STANDALONE_CFLAGS) -I$(SDL3_INC) -c -o $@ $<
# TinySoundFont implementation, isolated in its own object so the vendored header's warnings
# (-w) can't trip the standalone warning-free gate; sdl_audio.o gets the extern prototypes only. No SDL,
# no ROTH_STANDALONE — pure float DSP (linked with -lm via SDL_LDLIBS).
$(BUILD)/sdl_tsf.o: $(PLATFORM)/sdl/sdl_tsf.c $(PLATFORM)/viewer/tsf.h | $(BUILD)
	$(CC) $(CFLAGS) -w -c -o $@ $<
# the image-free boot: maps the arena + host video/DOS, obj3_build's the data, binds OS-API/host hooks
$(BUILD)/boot.o: $(PLATFORM)/boot.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
# the per-OS system seam, POSIX bodies (installs the SIGALRM tick handler; arms/retunes/stops the
# interval timer). Lives under platform/sys/, compiled plain like the other native host services.
$(BUILD)/sys_posix.o: $(PLATFORM)/sys/sys_posix.c $(PLATFORM)/sys/sys.h $(PLATFORM)/roth_host.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
# the per-OS system seam, Windows bodies (loader/alloc/protect/dir/arena/selector primitives; the
# periodic tick is a marked not-yet stub). Compiled plain; joins the link on the CROSS=mingw axis.
$(BUILD)/sys_win32.o: $(PLATFORM)/sys/sys_win32.c $(PLATFORM)/sys/sys.h $(PLATFORM)/roth_host.h $(PLATFORM)/win32_compat.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
# the Windows application icon/version resource -> COFF object. Compiled ONLY on CROSS=mingw and ONLY
# when res/rothc.ico exists (WIN_RES_OBJ is empty otherwise, so this rule never fires and the link
# carries no resource). Drop a Windows .ico at res/rothc.ico to brand the executable.
$(BUILD)/rothc_res.o: res/rothc.rc $(WIN_ICON) | $(BUILD)
	$(WINDRES) -I$(dir $(WIN_ICON)) $< -O coff -o $@

# --- the always-native host services (plain CFLAGS) ---
$(BUILD)/dos.o:                 $(PLATFORM)/dos.c                 | $(BUILD) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/dpmi.o:                $(PLATFORM)/dpmi.c                | $(BUILD) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/audio.o:               $(PLATFORM)/audio.c               | $(BUILD) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/audio_trace.o:         $(PLATFORM)/audio_trace.c         | $(BUILD) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/os_api.o:              $(PLATFORM)/os_api.c              | $(BUILD) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/standalone_hooks.o:    $(PLATFORM)/standalone_hooks.c    | $(BUILD) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/os_audio.o:            $(PLATFORM)/os_audio.c            | $(BUILD) ; $(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/os_audio_standalone.o: $(PLATFORM)/os_audio_standalone.c | $(BUILD) ; $(CC) $(CFLAGS) -c -o $@ $<

# --- there is no compile-time mod layer. keybinds + item_grabber + doc_viewer are runtime PLUGINS
# (sdk/examples/*), loaded by the platform below. There is no mods.o and no compile-time override rule.

# --- the runtime plugin platform — standalone only, needs the SDK public header ---
# plugin_loader.c: the loader/lifecycle state machine (plain CFLAGS, like boot.o; not sectioned).
$(BUILD)/plugin_loader.o: $(PLATFORM)/plugin_loader.c $(PLATFORM)/plugin_loader.h \
                          $(SDK_INC)/roth_sdk.h $(ENGINE)/engine.h $(PLATFORM)/standalone_hooks.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(SDK_INC) -c -o $@ $<
# host_adapters.c: the GENERATED host API table (the compile-time ABI proof, now linked for real).
# Compiled straight from sdk/generated/ — never hand-edited. It provides roth_host_api_v1 + the
# game_ram accessors (roth_game_ram_base/size are defined by plugin_loader.o).
$(BUILD)/host_adapters.o: $(SDK_DIR)/generated/host_adapters.c $(SDK_INC)/roth_sdk.h $(ENGINE)/engine.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(SDK_INC) -c -o $@ $<

# --- the override registry — standalone only ---
# override_registry.c: the chain builder + boot-time entry patcher (plain CFLAGS; not sectioned).
$(BUILD)/override_registry.o: $(PLATFORM)/override_registry.c $(PLATFORM)/override_registry.h \
                             $(SDK_INC)/roth_sdk.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(SDK_INC) -c -o $@ $<
# override_gen.c: GENERATED (gitignored) per-eligible-function dispatch thunks + the target table,
# regenerated from the checked-in eligibility manifest whenever it (or the generator) changes. The
# table references engine symbols by name (&fn) so the linker resolves the pad-entry addresses
# (strip-safe, no ELF self-inspection). NOT compiled with the pad flag (it is host dispatch code).
$(BUILD)/override_gen.c: $(SDK_DIR)/abi/override_targets.json $(SDK_DIR)/schema/functions.json \
                         $(TOOLS)/gen_override_dispatch.py | $(BUILD)
	$(PY) $(TOOLS)/gen_override_dispatch.py $@
# -DROTH_STANDALONE so engine.h exposes the standalone-only prototypes (the gdv host-video block) the
# table takes &fn of — matching how the engine TUs that DEFINE those symbols are compiled. No pad flag
# (this is host dispatch code, never a patch target).
$(BUILD)/override_gen.o: $(BUILD)/override_gen.c $(SDK_INC)/roth_sdk.h $(SDK_INC)/roth_sdk_overrides.h \
                         $(ENGINE)/engine.h $(PLATFORM)/override_registry.h | $(BUILD)
	$(CC) $(CFLAGS) -DROTH_STANDALONE -I$(SDK_INC) -c -o $@ $<

# --- datac data-as-C (sectioned so gc drops the probe/comparator helpers; no -DROTH_STANDALONE) ---
$(BUILD)/obj3_build_if.o: $(DATA)/obj3_build.c $(DATA)/obj3_data.h $(DATA)/obj3_owned.h | $(BUILD)
	$(CC) $(DATAC_CFLAGS) -c -o $@ $<
$(BUILD)/obj3_data_if.o:  $(DATA)/obj3_data.c  $(DATA)/obj3_data.h                      | $(BUILD)
	$(CC) $(DATAC_CFLAGS) -c -o $@ $<
$(BUILD)/obj3_owned_if.o: $(DATA)/obj3_owned.c $(DATA)/obj3_owned.h                     | $(BUILD)
	$(CC) $(DATAC_CFLAGS) -c -o $@ $<
$(BUILD)/obj1data_if.o:   $(DATA)/obj1data.c   $(DATA)/obj1data.h $(DATA)/obj1_owned.h  | $(BUILD)
	$(CC) $(DATAC_CFLAGS) -c -o $@ $<
$(BUILD)/obj1_owned_if.o: $(DATA)/obj1_owned.c $(DATA)/obj1_owned.h                     | $(BUILD)
	$(CC) $(DATAC_CFLAGS) -c -o $@ $<

# engine-header dependency for the lift TUs (correct incremental rebuilds)
$(STANDALONE_LIFT_OBJS): $(ENGINE_HDRS)

# clean removes BOTH flavors' artifacts (the current $(BUILD) tree + the vanilla tree + both binaries).
# The old artifact names are still removed (belt and braces) so a pre-rename tree cleans up too.
clean:
	rm -f roth roth-vanilla roth-host-imgfree roth-host-vanilla \
	      $(STANDALONE_HOST_OBJS) $(STANDALONE_LIFT_OBJS) \
	      $(PLUGIN_OBJS) $(OVERRIDE_OBJS) $(BUILD)/override_gen.c $(SDL_OBJS)
	rm -rf build-vanilla
	$(MAKE) -C $(PLATFORM)/viewer clean
