# sdk/examples/hello/Makefile — build the "hello" example plugin.
#
# The whole point of this build is that it compiles against ONLY the SDK public header
# (../../include/roth_sdk.h) — no engine headers, no repo headers — with the strict flags the
# SDK promises plugin authors. A drop-in plugin folder is: mods/hello/plugin.so.
CC      := gcc
SDK_INC := ../../include
CFLAGS  := -m32 -shared -fPIC -std=c11 -Wall -Wextra -Werror -I$(SDK_INC)

all: plugin.so

plugin.so: hello.c $(SDK_INC)/roth_sdk.h
	$(CC) $(CFLAGS) -o $@ hello.c

clean:
	rm -f plugin.so
