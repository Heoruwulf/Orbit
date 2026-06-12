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
#include <stdint.h>
#include <string.h>
#include "orbit/audio.h"
#include "vad_gru_weights.h"

struct vad_gru_state *vad_gru_init(struct audio_arena *const arena) {
    if (arena == nullptr) {
        return nullptr;
    }

    uintptr_t const addr   = (uintptr_t)(arena->buf + arena->curr);
    size_t const    offset = addr % 32;
    size_t const    pad    = (offset == 0) ? 0 : (32 - offset);

    if (pad > 0) {
        arena->curr += pad;
    }

    auto const state = (struct vad_gru_state *)arena_alloc(arena, sizeof(struct vad_gru_state));
    if (state == nullptr) {
        return nullptr;
    }

    vad_gru_reset(state);
    return state;
}

void vad_gru_reset(struct vad_gru_state *const state) {
    if (state != nullptr) {
        __builtin_memset(state->hidden_state, 0, sizeof(state->hidden_state));
    }
}

// AVX2 helper functions
static inline __m256 exp_ps(__m256 x) {
    x = _mm256_max_ps(x, _mm256_set1_ps(-88.0f));
    x = _mm256_min_ps(x, _mm256_set1_ps(88.0f));

    __m256 const log2e = _mm256_set1_ps(1.4426950408889634f);
    __m256 const y     = _mm256_mul_ps(x, log2e);

    __m256 const n = _mm256_round_ps(y, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256 const f = _mm256_sub_ps(y, n);

    __m256 const c1 = _mm256_set1_ps(0.6931471805599453f);
    __m256 const c2 = _mm256_set1_ps(0.2402265069591007f);
    __m256 const c3 = _mm256_set1_ps(0.0555041086648215f);
    __m256 const c4 = _mm256_set1_ps(0.0096181291076284f);
    __m256 const c5 = _mm256_set1_ps(0.0013333558146428f);

    __m256 p = c5;
    p        = _mm256_fmadd_ps(p, f, c4);
    p        = _mm256_fmadd_ps(p, f, c3);
    p        = _mm256_fmadd_ps(p, f, c2);
    p        = _mm256_fmadd_ps(p, f, c1);
    p        = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.0f));

    __m256i const imm   = _mm256_add_epi32(_mm256_cvtps_epi32(n), _mm256_set1_epi32(127));
    __m256 const  pow2n = _mm256_castsi256_ps(_mm256_slli_epi32(imm, 23));

    return _mm256_mul_ps(p, pow2n);
}

static inline __m256 sigmoid_ps(__m256 const x) {
    __m256 const neg_x = _mm256_sub_ps(_mm256_setzero_ps(), x);
    __m256 const den   = _mm256_add_ps(_mm256_set1_ps(1.0f), exp_ps(neg_x));
    return _mm256_div_ps(_mm256_set1_ps(1.0f), den);
}

static inline __m256 tanh_ps(__m256 const x) {
    __m256 const abs_x         = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), x);
    __m256 const neg_two_abs_x = _mm256_mul_ps(abs_x, _mm256_set1_ps(-2.0f));
    __m256 const exp_term      = exp_ps(neg_two_abs_x);

    __m256 const num = _mm256_sub_ps(_mm256_set1_ps(1.0f), exp_term);
    __m256 const den = _mm256_add_ps(_mm256_set1_ps(1.0f), exp_term);
    __m256 const res = _mm256_div_ps(num, den);

    __m256 const sign_mask = _mm256_and_ps(x, _mm256_set1_ps(-0.0f));
    return _mm256_or_ps(res, sign_mask);
}

static float vad_gru_process_frame_avx2(
    struct vad_gru_state *const state,
    float const *const restrict features,
    size_t const num_features) {
    if (state == nullptr || features == nullptr || num_features != VAD_FEATURE_DIM) {
        return 0.0f;
    }

    auto const h_prev = state->hidden_state;

    alignas(32) float input_proj[72];
    alignas(32) float rec_proj[72];

    __m256 const vx0 = _mm256_loadu_ps(&features[0]);
    __m256 const vx1 = _mm256_loadu_ps(&features[8]);
    __m128 const vx2 = _mm_loadu_ps(&features[16]);

    for (size_t i = 0; i < 72; ++i) {
        __m256 const vw0 = _mm256_loadu_ps(&VAD_GRU_W_INPUT[i * 20 + 0]);
        __m256 const vw1 = _mm256_loadu_ps(&VAD_GRU_W_INPUT[i * 20 + 8]);
        __m128 const vw2 = _mm_loadu_ps(&VAD_GRU_W_INPUT[i * 20 + 16]);

        __m256 sum256       = _mm256_mul_ps(vx0, vw0);
        sum256              = _mm256_fmadd_ps(vx1, vw1, sum256);
        __m128 const sum128 = _mm_mul_ps(vx2, vw2);

        __m128 const low      = _mm256_castps256_ps128(sum256);
        __m128 const high     = _mm256_extractf128_ps(sum256, 1);
        __m128       combined = _mm_add_ps(low, high);
        combined              = _mm_add_ps(combined, sum128);

        combined = _mm_hadd_ps(combined, combined);
        combined = _mm_hadd_ps(combined, combined);

        float dot_val;
        _mm_store_ss(&dot_val, combined);
        input_proj[i] = dot_val + VAD_GRU_B_INPUT[i];
    }

    __m256 const vh0 = _mm256_load_ps(&h_prev[0]);
    __m256 const vh1 = _mm256_load_ps(&h_prev[8]);
    __m256 const vh2 = _mm256_load_ps(&h_prev[16]);

    for (size_t i = 0; i < 72; ++i) {
        __m256 const vw0 = _mm256_loadu_ps(&VAD_GRU_W_RECURRENT[i * 24 + 0]);
        __m256 const vw1 = _mm256_loadu_ps(&VAD_GRU_W_RECURRENT[i * 24 + 8]);
        __m256 const vw2 = _mm256_loadu_ps(&VAD_GRU_W_RECURRENT[i * 24 + 16]);

        __m256 sum256 = _mm256_mul_ps(vh0, vw0);
        sum256        = _mm256_fmadd_ps(vh1, vw1, sum256);
        sum256        = _mm256_fmadd_ps(vh2, vw2, sum256);

        __m128 const low      = _mm256_castps256_ps128(sum256);
        __m128 const high     = _mm256_extractf128_ps(sum256, 1);
        __m128       combined = _mm_add_ps(low, high);

        combined = _mm_hadd_ps(combined, combined);
        combined = _mm_hadd_ps(combined, combined);

        float dot_val;
        _mm_store_ss(&dot_val, combined);
        rec_proj[i] = dot_val + VAD_GRU_B_RECURRENT[i];
    }

    alignas(32) float h_new[24];

    for (size_t g = 0; g < 3; ++g) {
        size_t const k_start = g * 8;

        __m256 const in_r  = _mm256_load_ps(&input_proj[k_start]);
        __m256 const rec_r = _mm256_load_ps(&rec_proj[k_start]);

        __m256 const in_z  = _mm256_load_ps(&input_proj[24 + k_start]);
        __m256 const rec_z = _mm256_load_ps(&rec_proj[24 + k_start]);

        __m256 const in_n  = _mm256_load_ps(&input_proj[48 + k_start]);
        __m256 const rec_n = _mm256_load_ps(&rec_proj[48 + k_start]);

        __m256 const prev_h = _mm256_load_ps(&h_prev[k_start]);

        __m256 const r = sigmoid_ps(_mm256_add_ps(in_r, rec_r));
        __m256 const z = sigmoid_ps(_mm256_add_ps(in_z, rec_z));
        __m256 const n = tanh_ps(_mm256_fmadd_ps(r, rec_n, in_n));

        __m256 const one         = _mm256_set1_ps(1.0f);
        __m256 const one_minus_z = _mm256_sub_ps(one, z);
        __m256 const new_h       = _mm256_fmadd_ps(one_minus_z, n, _mm256_mul_ps(z, prev_h));

        _mm256_store_ps(&h_new[k_start], new_h);
    }

    __builtin_memcpy(state->hidden_state, h_new, sizeof(h_new));

    __m256 const vh_new0 = _mm256_load_ps(&h_new[0]);
    __m256 const vh_new1 = _mm256_load_ps(&h_new[8]);
    __m256 const vh_new2 = _mm256_load_ps(&h_new[16]);

    __m256 const vfc_w0 = _mm256_loadu_ps(&VAD_FC_WEIGHT[0]);
    __m256 const vfc_w1 = _mm256_loadu_ps(&VAD_FC_WEIGHT[8]);
    __m256 const vfc_w2 = _mm256_loadu_ps(&VAD_FC_WEIGHT[16]);

    __m256 sum256 = _mm256_mul_ps(vh_new0, vfc_w0);
    sum256        = _mm256_fmadd_ps(vh_new1, vfc_w1, sum256);
    sum256        = _mm256_fmadd_ps(vh_new2, vfc_w2, sum256);

    __m128 const low      = _mm256_castps256_ps128(sum256);
    __m128 const high     = _mm256_extractf128_ps(sum256, 1);
    __m128       combined = _mm_add_ps(low, high);

    combined = _mm_hadd_ps(combined, combined);
    combined = _mm_hadd_ps(combined, combined);

    float fc_val;
    _mm_store_ss(&fc_val, combined);

    float const out_val = fc_val + VAD_FC_BIAS[0];
    return 1.0f / (1.0f + __builtin_expf(-out_val));
}

float vad_gru_process_pcm(
    struct vad_gru_state *const state,
    int16_t const *const        pcm,
    size_t const                samples,
    int const                   sample_rate,
    int const                   channels) {
    if (state == nullptr || pcm == nullptr || samples < 1 || channels <= 0) {
        return -1.0f;
    }

    if (sample_rate != VAD_SAMPLE_RATE) {
        return -1.0f;
    }

    size_t const frame_size = (size_t)(sample_rate * 0.02);
    if (frame_size == 0) {
        return -1.0f;
    }

    float max_prob = 0.0f;
    float features[VAD_FEATURE_DIM];

    for (size_t offset = 0; offset + frame_size <= samples; offset += frame_size) {
        size_t const seg_size = frame_size / VAD_FEATURE_DIM;
        if (seg_size == 0) {
            continue;
        }

        for (size_t g = 0; g < VAD_FEATURE_DIM; ++g) {
            float energy = 0.0f;
            for (size_t s = 0; s < seg_size; ++s) {
                float val = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    val += (float)pcm[(offset + g * seg_size + s) * (size_t)channels + (size_t)c];
                }
                val /= (float)channels;
                energy += __builtin_fabsf(val) / 32768.0f;
            }
            energy /= (float)seg_size;
            features[g] = __builtin_logf(1.0f + energy * 10.0f);
        }

        float const prob = vad_gru_process_frame_avx2(state, features, VAD_FEATURE_DIM);
        if (prob > max_prob) {
            max_prob = prob;
        }
    }

    return max_prob;
}
