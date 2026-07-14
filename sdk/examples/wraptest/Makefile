# sdk/examples/wraptest/Makefile — build the "wraptest" override-registry example plugin.
#
# Compiles against ONLY the SDK public header (../../include/roth_sdk.h) with the strict flags the
# SDK promises plugin authors. Drop-in plugin folder: mods/wraptest/plugin.so.
CC      := gcc
SDK_INC := ../../include
CFLAGS  := -m32 -shared -fPIC -std=c11 -Wall -Wextra -Werror -I$(SDK_INC)

all: plugin.so

plugin.so: wraptest.c $(SDK_INC)/roth_sdk.h $(SDK_INC)/roth_sdk_overrides.h
	$(CC) $(CFLAGS) -o $@ wraptest.c

clean:
	rm -f plugin.so
