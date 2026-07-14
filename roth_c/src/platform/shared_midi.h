/* Shared-memory MIDI-event ring between the 32-bit host and the SDL2 viewer.
 *
 * The host captures the GM messages the game hands its (virtualized) MIDI driver
 * — note on/off, program change, controllers, pitch bend — and pushes them here;
 * the viewer drains them into a SoundFont synth (TinySoundFont) and mixes the
 * result into its SDL audio output. Separate from the PCM ring (shared_audio.h):
 * that one carries the digital mixer's PCM (SFX/voice/cutscenes); this one is the
 * MIDI music stream. Single-producer (host) / single-consumer (viewer).
 */
#ifndef ROTH_SHARED_MIDI_H
#define ROTH_SHARED_MIDI_H

#include <stdint.h>

#define ROTH_MIDI_SHM_NAME "/roth_midi"
#define ROTH_MIDI_MAGIC    0x4944494du /* 'MIDI' */
#define ROTH_MIDI_RING     8192u       /* events (power of 2) */
#define ROTH_MIDI_MASK     (ROTH_MIDI_RING - 1u)

struct roth_midi_ev {
    uint8_t status; /* command | channel (0x80..0xEF) */
    uint8_t d1;
    uint8_t d2;
    uint8_t pad;
};

struct roth_midi {
    uint32_t magic;
    volatile uint32_t ready;     /* host set up the ring */
    volatile uint32_t w;         /* producer (host) event count */
    volatile uint32_t r;         /* consumer (viewer) event count */
    struct roth_midi_ev ev[ROTH_MIDI_RING];
};

#endif
