/* Shared-memory PCM ring between the 32-bit host and the SDL2 viewer.
 *
 * The host taps the HMI software mixer's output (the per-tick digital "poll"
 * callback, audio.c) and writes PCM bytes into this ring; the viewer opens an
 * SDL audio device and its callback drains the ring. Kept separate from the
 * framebuffer shm (shared_fb.h) so the audio workstream owns it cleanly.
 *
 * Single-producer (host) / single-consumer (viewer), lock-free: the producer
 * owns `w`, the consumer owns `r`, both monotonically increasing byte counts.
 */
#ifndef ROTH_SHARED_AUDIO_H
#define ROTH_SHARED_AUDIO_H

#include <stdint.h>

#define ROTH_AUDIO_SHM_NAME "/roth_audio"
#define ROTH_AUDIO_MAGIC    0x4f445541u /* 'AUDO' */
#define ROTH_AUDIO_RING     (1u << 17)  /* 128 KB PCM ring */
#define ROTH_AUDIO_MASK     (ROTH_AUDIO_RING - 1u)

struct roth_audio {
    uint32_t magic;
    volatile uint32_t rate;     /* sample rate in Hz (host sets) */
    volatile uint32_t channels; /* 1 = mono, 2 = stereo */
    volatile uint32_t bits;     /* 8 = unsigned, 16 = signed LE */
    volatile uint32_t ready;    /* nonzero once the host has set the format */
    volatile uint32_t w;        /* producer (host) byte write count */
    volatile uint32_t r;        /* consumer (viewer) byte read count */
    volatile uint32_t underruns;
    uint8_t ring[ROTH_AUDIO_RING];
};

#endif
