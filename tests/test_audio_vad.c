/*
Orbit: High-performance, zero-allocation bi-directional audio bridge.
Copyright (C) 2026 Mark Horila

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <assert.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "orbit/audio.h"

int main(void) {
    alignas(32) uint8_t arena_buf[1024];
    struct audio_arena  arena = {.buf = arena_buf, .curr = 0, .capacity = sizeof(arena_buf)};

    struct vad_gru_state *const state = vad_gru_init(&arena);
    assert(state != nullptr);

    // Test silence (prob should be very low)
    alignas(32) int16_t silence[320] = {0}; // 20ms at 16kHz
    float const         prob_silence = vad_gru_process_pcm(state, silence, 320, 16000, 1);
    printf("prob_silence: %f\n", prob_silence);
    assert(prob_silence >= 0.0f && prob_silence < 0.3f);

    // Reset state
    vad_gru_reset(state);

    // Test fake speech (high energy sine wave)
    alignas(32) int16_t speech[320];
    for (size_t i = 0; i < 320; ++i) {
        speech[i] = (int16_t)(__builtin_sinf((float)i * 0.1f) * 16000.0f);
    }

    // Pump a few frames to warm up the GRU
    vad_gru_process_pcm(state, speech, 320, 16000, 1);
    vad_gru_process_pcm(state, speech, 320, 16000, 1);
    float const prob_speech = vad_gru_process_pcm(state, speech, 320, 16000, 1);

    printf("prob_speech: %f\n", prob_speech);
    assert(prob_speech > prob_silence);

    return EXIT_SUCCESS;
}
