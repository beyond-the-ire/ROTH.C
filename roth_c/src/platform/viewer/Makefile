# 64-bit SDL2 viewer (uses the system SDL2; the game host stays 32-bit).
# The shared framebuffer/audio/midi headers live one level up in the platform layer.
CC      := gcc
CFLAGS  := -O2 -g -Wall -Wextra $(shell pkg-config --cflags sdl2)
LDFLAGS := $(shell pkg-config --libs sdl2) -lrt

all: roth-view roth-inject

roth-view: viewer.c ../shared_fb.h ../shared_audio.h ../shared_midi.h tsf.h
	$(CC) $(CFLAGS) -o $@ viewer.c $(LDFLAGS) -lm

# headless shm helper (scancode inject / framebuffer probe) — no SDL, only -lrt for shm_open
roth-inject: inject_test.c ../shared_fb.h
	$(CC) -O2 -g -o $@ inject_test.c -lrt

clean:
	rm -f roth-view roth-inject

.PHONY: all clean
