#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

constexpr size_t RESAMPLER_MAX_TAPS = 256;

struct audio_resampler {
    int            phases;
    int            taps;
    int16_t const *coeffs;
    int64_t        pos_fp;
    int16_t        delay_buf[RESAMPLER_MAX_TAPS];
};

struct audio_arena {
    uint8_t *buf;
    size_t   curr;
    size_t   capacity;
};

static inline void *arena_alloc(struct audio_arena *const a, size_t const size) {
    uintptr_t const addr   = (uintptr_t)(a->buf + a->curr);
    size_t const    offset = addr % 32;
    size_t const    pad    = (offset == 0) ? 0 : (32 - offset);
    if (a->curr + pad + size > a->capacity) {
        return nullptr;
    }
    a->curr += pad;
    void *ptr = a->buf + a->curr;
    a->curr += size;
    return ptr;
}

int resampler_init(
    struct audio_resampler *const r,
    uint32_t const                in_rate,
    uint32_t const                out_rate);

size_t resample_l16_advanced(
    struct audio_resampler *const r,
    uint32_t const                in_rate,
    uint32_t const                out_rate,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    int16_t *restrict const out_data,
    size_t const out_capacity);

constexpr size_t VAD_FEATURE_DIM = 20;
constexpr size_t VAD_HIDDEN_DIM  = 24;
constexpr int    VAD_SAMPLE_RATE = 16000;

struct vad_gru_state {
    alignas(32) float hidden_state[VAD_HIDDEN_DIM];
};

struct vad_gru_state *vad_gru_init(struct audio_arena *const arena);
void                  vad_gru_reset(struct vad_gru_state *const state);
float                 vad_gru_process_pcm(
                    struct vad_gru_state *const state,
                    int16_t const *const        pcm,
                    size_t const                samples,
                    int const                   sample_rate,
                    int const                   channels);

enum audio_codec { CODEC_PASS, CODEC_PCMU, CODEC_PCMA, CODEC_L16 };

enum audio_endian { ENDIAN_NONE, ENDIAN_LITTLE, ENDIAN_BIG };

struct transcode_config {
    enum audio_codec  in_codec;
    uint32_t          in_sample_rate;
    uint32_t          in_channels;
    enum audio_endian in_endian;

    enum audio_codec  out_codec;
    uint32_t          out_sample_rate;
    uint32_t          out_channels;
    enum audio_endian out_endian;

    bool vad_enabled;
};

struct transcode_session {
    enum audio_codec  in_codec;
    uint32_t          in_sample_rate;
    uint32_t          in_channels;
    enum audio_endian in_endian;

    enum audio_codec  out_codec;
    uint32_t          out_sample_rate;
    uint32_t          out_channels;
    enum audio_endian out_endian;

    bool swap_in;
    bool swap_out;
    bool needs_resample;
    bool vad_enabled;

    struct audio_resampler resampler;
    struct vad_gru_state  *vad_state;
    float                  last_vad_prob;
};

int transcode_session_init(
    struct transcode_session *const      session,
    struct transcode_config const *const config,
    struct audio_arena *const            vad_arena);

int transcode_process(
    struct transcode_session *const session,
    uint8_t const *const            in_data,
    size_t const                    in_len,
    struct audio_arena *const       out_arena,
    uint8_t **const                 out_data,
    size_t *const                   out_len);
