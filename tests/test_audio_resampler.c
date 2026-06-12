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
#include <math.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include "orbit/audio.h"

constexpr size_t IN_SAMPLES  = 8000;  // 1 second at 8kHz
constexpr size_t OUT_SAMPLES = 16000; // 1 second at 16kHz

int main(void) {
    struct audio_resampler r;
    int const              init_res = resampler_init(&r, 8000, 16000);
    assert(init_res == 0);

    alignas(32) int16_t in_data[IN_SAMPLES];
    alignas(32) int16_t out_data[OUT_SAMPLES + 1024];

    // Generate 440Hz sine wave
    for (size_t i = 0; i < IN_SAMPLES; ++i) {
        double const t = (double)i / 8000.0;
        in_data[i]     = (int16_t)(sin(2.0 * 3.14159265358979323846 * 440.0 * t) * 16384.0);
    }

    size_t const written =
        resample_l16_advanced(&r, 8000, 16000, in_data, IN_SAMPLES, out_data, OUT_SAMPLES + 1024);
    assert(written == OUT_SAMPLES);

    // Verify non-zero energy in output
    double energy = 0;
    for (size_t i = 0; i < OUT_SAMPLES; ++i) {
        energy += (double)out_data[i] * (double)out_data[i];
    }
    assert(energy > 1e6); // basic sanity check

    // Check downsampling (16000 to 8000)
    struct audio_resampler r2;
    int const              init_res2 = resampler_init(&r2, 16000, 8000);
    assert(init_res2 == 0);

    alignas(32) int16_t down_data[IN_SAMPLES + 1024];
    size_t const        down_written = resample_l16_advanced(
        &r2,
        16000,
        8000,
        out_data,
        OUT_SAMPLES,
        down_data,
        IN_SAMPLES + 1024);
    assert(down_written == IN_SAMPLES);

    return EXIT_SUCCESS;
}
