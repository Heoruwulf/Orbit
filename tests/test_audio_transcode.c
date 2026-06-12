#include <assert.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include "orbit/audio.h"

int main(void) {
    alignas(32) uint8_t arena_buf[16384];
    struct audio_arena  arena = {.buf = arena_buf, .curr = 0, .capacity = sizeof(arena_buf)};

    struct transcode_config cfg = {
        .in_codec       = CODEC_PCMU,
        .in_sample_rate = 8000,
        .in_channels    = 1,
        .in_endian      = ENDIAN_NONE,

        .out_codec       = CODEC_L16,
        .out_sample_rate = 16000,
        .out_channels    = 1,
        .out_endian      = ENDIAN_LITTLE,

        .vad_enabled = true};

    struct transcode_session session;
    int const                init_res = transcode_session_init(&session, &cfg, &arena);
    assert(init_res == 0);

    // Generate PCMU silence (0xFF is silence in u-law)
    uint8_t pcmu_in[160];
    for (size_t i = 0; i < 160; ++i) {
        pcmu_in[i] = 0xFF;
    }

    uint8_t *out_data = nullptr;
    size_t   out_len  = 0;

    int const proc_res = transcode_process(&session, pcmu_in, 160, &arena, &out_data, &out_len);
    assert(proc_res == 0);
    assert(out_data != nullptr);
    assert(out_len == 640); // 160 samples at 8kHz resampled to 320 samples at 16kHz. L16 is 2 bytes
                            // per sample -> 640 bytes.

    // Check VAD state
    assert(session.last_vad_prob >= 0.0f && session.last_vad_prob < 0.3f); // Expected silence

    return EXIT_SUCCESS;
}
