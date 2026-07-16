# sdk/examples/wraptest/Makefile — build the "wraptest" override-registry example plugin.
#
# Compiles against ONLY the SDK public header (../../include/roth_sdk.h) with the strict flags the
# SDK promises plugin authors. Drop-in plugin folder: mods/wraptest/plugin.so.
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

$(LIB): wraptest.c $(SDK_INC)/roth_sdk.h $(SDK_INC)/roth_sdk_overrides.h
	$(CC) $(CFLAGS) -o $@ wraptest.c

clean:
	rm -f plugin.so plugin.dll
