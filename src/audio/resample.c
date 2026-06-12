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
#include <immintrin.h>
#include <math.h>
#include <stdalign.h>
#include <string.h>
#include "orbit/audio.h"
#include "orbit/macros.h"

#include "resample_sinc_table.h"

static inline int16_t clamp_to_int16(double const val) {
    double const r = round(val);
    if (r > 32767.0)
        return 32767;
    if (r < -32768.0)
        return -32768;
    return (int16_t)r;
}

static inline float
mac_128_taps_avx2(int16_t const *restrict const in, int16_t const *restrict const coeffs) {
    __m256 v_sum0 = _mm256_setzero_ps();
    __m256 v_sum1 = _mm256_setzero_ps();

    for (int i = 0; i < 128; i += 32) {
        __m256i const v_in0 = _mm256_loadu_si256((__m256i const *)&in[i]);
        __m256i const v_c0  = _mm256_load_si256((__m256i const *)&coeffs[i]);
        __m256i const v_m0  = _mm256_madd_epi16(v_in0, v_c0);
        v_sum0              = _mm256_add_ps(v_sum0, _mm256_cvtepi32_ps(v_m0));

        __m256i const v_in1 = _mm256_loadu_si256((__m256i const *)&in[i + 16]);
        __m256i const v_c1  = _mm256_load_si256((__m256i const *)&coeffs[i + 16]);
        __m256i const v_m1  = _mm256_madd_epi16(v_in1, v_c1);
        v_sum1              = _mm256_add_ps(v_sum1, _mm256_cvtepi32_ps(v_m1));
    }

    __m256 const v_sum = _mm256_add_ps(v_sum0, v_sum1);

    __m128       v_low  = _mm256_castps256_ps128(v_sum);
    __m128 const v_high = _mm256_extractf128_ps(v_sum, 1);
    v_low               = _mm_add_ps(v_low, v_high);
    v_low               = _mm_hadd_ps(v_low, v_low);
    v_low               = _mm_hadd_ps(v_low, v_low);

    return _mm_cvtss_f32(v_low);
}

static inline void mac_downsample_avx2(
    struct audio_resampler *restrict const r,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    int16_t *restrict const out_data,
    size_t const  count,
    double const  ratio,
    int64_t const step_fp) {

    double const s          = 1.0 / ratio;
    double const step       = s;
    int const    center_off = (SINC_TAPS / 2) - 1;
    __m256 const v_norm     = _mm256_set1_ps((float)ratio / 32767.0f);

    for (size_t i = 0; i < count; ++i) {
        int64_t const pos_fp = r->pos_fp;
        double const  t0 = (double)(pos_fp >> 32) + (double)(pos_fp & 0xFFFFFFFF) / 4294967296.0;
        double const  t7 = t0 + 7.0 * step;
        int const     k_min_all = (int)ceil(t0 - s * (double)center_off);
        int const     k_max_all = (int)floor(t7 + s * (double)(SINC_TAPS - 1 - center_off));

        __m256 v_acc = _mm256_setzero_ps();

#define LOOP_BODY(GET_SAMPLE)                                                                      \
    do {                                                                                           \
        double const val_idx0 = (t0 - (double)k) * ratio + (double)center_off;                     \
        int const    j0       = (int)floor(val_idx0);                                              \
        double const alpha0   = val_idx0 - (double)j0;                                             \
        int          pp       = (int)(alpha0 * (double)SINC_PHASES);                               \
        if (unlikely(pp >= SINC_PHASES))                                                           \
            pp = SINC_PHASES - 1;                                                                  \
        else if (unlikely(pp < 0))                                                                 \
            pp = 0;                                                                                \
        __m128i v_c16_load;                                                                        \
        if (unlikely(j0 < 0 || j0 > SINC_TAPS - 8)) {                                              \
            alignas(16) int16_t tmp[8];                                                            \
            for (int m = 0; m < 8; ++m) {                                                          \
                int const j_m = j0 + m;                                                            \
                tmp[m]        = (j_m >= 0 && j_m < SINC_TAPS) ? SINC_COEFFS[pp][j_m] : 0;          \
            }                                                                                      \
            v_c16_load = _mm_load_si128((__m128i const *)tmp);                                     \
        } else {                                                                                   \
            v_c16_load = _mm_loadu_si128((__m128i const *)&SINC_COEFFS[pp][j0]);                   \
        }                                                                                          \
        __m256i const v_c32  = _mm256_cvtepi16_epi32(v_c16_load);                                  \
        __m256 const  v_c_ps = _mm256_cvtepi32_ps(v_c32);                                          \
        int16_t const sample = GET_SAMPLE;                                                         \
        v_acc                = _mm256_fmadd_ps(_mm256_set1_ps((float)sample), v_c_ps, v_acc);      \
    } while (0)

        int k = k_min_all;
        for (; k < 0 && k <= k_max_all; ++k) {
            LOOP_BODY(
                r->delay_buf[(int)RESAMPLER_MAX_TAPS + k < 0 ? 0 : (int)RESAMPLER_MAX_TAPS + k]);
        }
        for (; k < (int)in_samples && k <= k_max_all; ++k) {
            LOOP_BODY(in_data[k]);
        }
        for (; k <= k_max_all; ++k) {
            LOOP_BODY(in_data[in_samples - 1]);
        }
#undef LOOP_BODY

        v_acc                 = _mm256_mul_ps(v_acc, v_norm);
        __m256i const v_out32 = _mm256_cvtps_epi32(
            _mm256_round_ps(v_acc, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
        __m256i const v_packed   = _mm256_packs_epi32(v_out32, v_out32);
        __m256i const v_permuted = _mm256_permute4x64_epi64(v_packed, _MM_SHUFFLE(3, 2, 2, 0));
        _mm_storeu_si128((__m128i *)&out_data[i * 8], _mm256_castsi256_si128(v_permuted));

        r->pos_fp += 8 * step_fp;
    }
}

static inline bool is_rate_supported(uint32_t const rate) {
    return rate == 8000 || rate == 16000 || rate == 24000 || rate == 32000 || rate == 44100 ||
           rate == 48000;
}

int resampler_init(
    struct audio_resampler *const r,
    uint32_t const                in_rate,
    uint32_t const                out_rate) {
    if (!is_rate_supported(in_rate) || !is_rate_supported(out_rate)) {
        return -1;
    }

    r->phases = 0;
    r->taps   = 2;
    r->coeffs = nullptr;
    double s  = (double)in_rate / (double)out_rate;
    if (s < 1.0) {
        s = 1.0;
    }
    int64_t const delay = (int64_t)ceil(s * 64.0);
    r->pos_fp           = -(delay << 32);

    __builtin_memset(r->delay_buf, 0, sizeof(r->delay_buf));
    return 0;
}

size_t resample_l16_advanced(
    struct audio_resampler *const r,
    uint32_t const                in_rate,
    uint32_t const                out_rate,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    int16_t *restrict const out_data,
    size_t const out_capacity) {

    if (unlikely(in_rate == out_rate)) {
        size_t const samples = in_samples < out_capacity ? in_samples : out_capacity;
        __builtin_memcpy(out_data, in_data, samples * sizeof(int16_t));
        return samples;
    }

    double const  ratio   = (double)out_rate / (double)in_rate;
    double const  step    = 1.0 / ratio;
    int64_t const step_fp = (int64_t)(step * (double)(1ULL << 32));

    size_t const out_samples = (size_t)((double)in_samples * ratio);
    if (unlikely(out_samples > out_capacity)) {
        return 0;
    }

    int const    center_off = (SINC_TAPS / 2) - 1;
    double const s          = ratio < 1.0 ? 1.0 / ratio : 1.0;
    double const inv_s      = 1.0 / s;

    for (size_t i = 0; i < out_samples; ++i) {
        int64_t const pos_fp = r->pos_fp;
        int const     idx    = (int)(pos_fp >> 32);
        double const  frac   = (double)(pos_fp & 0xFFFFFFFF) / (double)(1ULL << 32);

        double dacc = 0.0;

        if (likely(ratio >= 1.0)) {
            int const            phase  = (int)(frac * SINC_PHASES);
            int const            p      = phase >= SINC_PHASES ? SINC_PHASES - 1 : phase;
            int16_t const *const coeffs = SINC_COEFFS[p];

            if (likely(idx >= center_off && idx < (int)in_samples - (SINC_TAPS - center_off))) {
                int16_t const *const in_ptr = &in_data[idx - center_off];
                float const          acc    = mac_128_taps_avx2(in_ptr, coeffs);
                dacc                        = (double)acc / 32767.0;
            } else {
                alignas(32) int16_t pad_buf[SINC_TAPS];
                for (int j = 0; j < SINC_TAPS; ++j) {
                    int const k = (int)idx - center_off + j;
                    if (k < 0) {
                        pad_buf[j] = r->delay_buf[(int)RESAMPLER_MAX_TAPS + k];
                    } else if (k >= (int)in_samples) {
                        pad_buf[j] = in_data[in_samples - 1];
                    } else {
                        pad_buf[j] = in_data[k];
                    }
                }
                float const acc = mac_128_taps_avx2(pad_buf, coeffs);
                dacc            = (double)acc / 32767.0;
            }
        } else {
            size_t const remaining   = out_samples - i;
            size_t const avx8_blocks = remaining / 8;
            if (avx8_blocks > 0) {
                mac_downsample_avx2(
                    r,
                    in_data,
                    in_samples,
                    &out_data[i],
                    avx8_blocks,
                    ratio,
                    step_fp);
                i += avx8_blocks * 8 - 1;
                continue;
            }

            double const t     = (double)idx + frac;
            int const    k_min = (int)ceil(t - s * (double)center_off);
            int const    k_max = (int)floor(t + s * (double)(SINC_TAPS - 1 - center_off));

            for (int k = k_min; k <= k_max; ++k) {
                double const x_target = (t - (double)k) * inv_s;
                double const val_idx  = x_target + (double)center_off;
                int const    j        = (int)floor(val_idx);
                double const alpha    = val_idx - (double)j;

                if (j >= 0 && j < SINC_TAPS) {
                    int const p  = (int)(alpha * SINC_PHASES);
                    int const pp = p >= SINC_PHASES ? SINC_PHASES - 1 : p;

                    int16_t sample;
                    if (k < 0) {
                        sample =
                            r->delay_buf
                                [(int)RESAMPLER_MAX_TAPS + k < 0 ? 0 : (int)RESAMPLER_MAX_TAPS + k];
                    } else if (k >= (int)in_samples) {
                        sample = in_data[in_samples - 1];
                    } else {
                        sample = in_data[k];
                    }
                    dacc += (double)sample * (double)SINC_COEFFS[pp][j];
                }
            }
            dacc = (dacc / 32767.0) * ratio;
        }

        out_data[i] = clamp_to_int16(dacc);
        r->pos_fp += step_fp;
    }

    if (in_samples >= RESAMPLER_MAX_TAPS) {
        __builtin_memcpy(
            r->delay_buf,
            &in_data[in_samples - RESAMPLER_MAX_TAPS],
            RESAMPLER_MAX_TAPS * sizeof(int16_t));
    } else {
        __builtin_memmove(
            r->delay_buf,
            &r->delay_buf[in_samples],
            (RESAMPLER_MAX_TAPS - in_samples) * sizeof(int16_t));
        __builtin_memcpy(
            &r->delay_buf[RESAMPLER_MAX_TAPS - in_samples],
            in_data,
            in_samples * sizeof(int16_t));
    }

    r->pos_fp -= ((int64_t)in_samples << 32);
    return out_samples;
}
