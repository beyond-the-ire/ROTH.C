# sdk/examples/hello/Makefile — build the "hello" example plugin.
#
# The whole point of this build is that it compiles against ONLY the SDK public header
# (../../include/roth_sdk.h) — no engine headers, no repo headers — with the strict flags the
# SDK promises plugin authors. A drop-in plugin folder is: mods/hello/plugin.so.
#
# CROSS unset builds the native Linux plugin.so; CROSS=mingw cross-builds a Windows plugin.dll
# with the MinGW i686 compiler. -fPIC is ELF-only (PE relocates on its own), so the mingw branch
# drops it; everything else is identical.
SDK_INC := ../../include
ifeq ($(CROSS),mingw)
  CC     := i686-w64-mingw32-gcc
  LIB    := plugin.dll
  CFLAGS := -shared -std=c11 -Wall -Wextra -Werror -I$(SDK_INC)
else
  CC     := gcc
  LIB    := plugin.so
  CFLAGS := -m32 -shared -fPIC -std=c11 -Wall -Wextra -Werror -I$(SDK_INC)
endif

all: $(LIB)

$(LIB): hello.c $(SDK_INC)/roth_sdk.h
	$(CC) $(CFLAGS) -o $@ hello.c

clean:
	rm -f plugin.so plugin.dll
