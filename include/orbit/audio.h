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
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Maximum number of taps (coefficients) supported by the resampler.
 */
constexpr size_t RESAMPLER_MAX_TAPS = 256;

/**
 * @brief Polyphase Sinc Resampler state.
 */
struct audio_resampler {
    int            phases;                 /**< Number of polyphase filters. */
    int            taps;                   /**< Number of coefficients per phase. */
    int16_t const *coeffs;                 /**< Pointer to the coefficient table. */
    int64_t        pos_fp;                 /**< Current fractional position in the stream. */
    int16_t delay_buf[RESAMPLER_MAX_TAPS]; /**< History buffer for overlapping convolution. */
};

/**
 * @brief A lock-free, zero-allocation memory arena for audio buffers.
 */
struct audio_arena {
    uint8_t *buf;      /**< Pointer to the pre-allocated backing buffer. */
    size_t   curr;     /**< Current byte offset in the buffer. */
    size_t   capacity; /**< Maximum size of the buffer in bytes. */
};

/**
 * @brief Allocates 32-byte aligned memory from an audio arena.
 *
 * @param a Pointer to the audio arena.
 * @param size Number of bytes to allocate.
 * @return Pointer to the allocated memory, or nullptr if out of capacity.
 */
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

/**
 * @brief Initializes a polyphase sinc resampler.
 *
 * @param r Pointer to the resampler state to initialize.
 * @param in_rate Input sample rate (Hz).
 * @param out_rate Output sample rate (Hz).
 * @return 0 on success, or -1 if the conversion ratio is unsupported.
 */
int resampler_init(
    struct audio_resampler *const r,
    uint32_t const                in_rate,
    uint32_t const                out_rate);

/**
 * @brief Resamples an array of 16-bit linear PCM audio.
 *
 * @param r Pointer to the initialized resampler state.
 * @param in_rate Input sample rate (Hz).
 * @param out_rate Output sample rate (Hz).
 * @param in_data Pointer to the input PCM data.
 * @param in_samples Number of input samples.
 * @param out_data Pointer to the output PCM buffer.
 * @param out_capacity Maximum number of samples the output buffer can hold.
 * @return The number of samples written to the output buffer.
 */
size_t resample_l16_advanced(
    struct audio_resampler *const r,
    uint32_t const                in_rate,
    uint32_t const                out_rate,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    int16_t *restrict const out_data,
    size_t const out_capacity);

/** @brief Number of Mel-frequency cepstral coefficients used by VAD. */
constexpr size_t VAD_FEATURE_DIM = 20;

/** @brief Dimension of the GRU hidden state. */
constexpr size_t VAD_HIDDEN_DIM = 24;

/** @brief The expected sample rate for VAD input. */
constexpr int VAD_SAMPLE_RATE = 16000;

/**
 * @brief Gated Recurrent Unit (GRU) state for Voice Activity Detection.
 */
struct vad_gru_state {
    alignas(32) float hidden_state[VAD_HIDDEN_DIM]; /**< The hidden memory vector. */
};

/**
 * @brief Initializes a new VAD GRU state from the given arena.
 *
 * @param arena Pointer to the audio arena.
 * @return Pointer to the initialized state, or nullptr on failure.
 */
struct vad_gru_state *vad_gru_init(struct audio_arena *restrict const arena);

/**
 * @brief Resets the hidden state of the VAD.
 *
 * @param state Pointer to the GRU state.
 */
void vad_gru_reset(struct vad_gru_state *restrict const state);

/**
 * @brief Processes a frame of audio and returns the probability of voice activity.
 *
 * @param state Pointer to the GRU state.
 * @param pcm Pointer to the interleaved 16-bit PCM audio.
 * @param samples Number of total samples.
 * @param sample_rate The sample rate of the audio.
 * @param channels The number of channels.
 * @return A probability between 0.0f and 1.0f indicating voice activity, or -1.0f on error.
 */
// clang-format off
float vad_gru_process_pcm(
    struct vad_gru_state *restrict const state,
    int16_t const *restrict const        pcm,
    size_t const                         samples,
    int const                            sample_rate,
    int const                            channels);
// clang-format on

/**
 * @brief Supported audio codecs for transcoding.
 */
enum audio_codec {
    CODEC_PASS, /**< Pass-through (no transcoding). */
    CODEC_PCMU, /**< G.711 u-law (PCMU). */
    CODEC_PCMA, /**< G.711 A-law (PCMA). */
    CODEC_L16   /**< 16-bit linear PCM. */
};

/**
 * @brief Endianness for 16-bit audio codecs.
 */
enum audio_endian {
    ENDIAN_NONE,   /**< Not applicable (8-bit codecs). */
    ENDIAN_LITTLE, /**< Little-endian (standard for WebRTC). */
    ENDIAN_BIG     /**< Big-endian (network byte order). */
};

/**
 * @brief Configuration parameters for a transcoding session.
 */
struct transcode_config {
    enum audio_codec  in_codec;       /**< Input codec. */
    uint32_t          in_sample_rate; /**< Input sample rate (Hz). */
    uint32_t          in_channels;    /**< Number of input channels. */
    enum audio_endian in_endian;      /**< Endianness of input audio (if L16). */

    enum audio_codec  out_codec;       /**< Output codec. */
    uint32_t          out_sample_rate; /**< Output sample rate (Hz). */
    uint32_t          out_channels;    /**< Number of output channels. */
    enum audio_endian out_endian;      /**< Endianness of output audio (if L16). */

    bool vad_enabled; /**< If true, VAD will be calculated on the stream. */
};

/**
 * @brief State container for an active audio transcoding session.
 */
struct transcode_session {
    enum audio_codec  in_codec;       /**< Input codec. */
    uint32_t          in_sample_rate; /**< Input sample rate (Hz). */
    uint32_t          in_channels;    /**< Number of input channels. */
    enum audio_endian in_endian;      /**< Input endianness. */

    enum audio_codec  out_codec;       /**< Output codec. */
    uint32_t          out_sample_rate; /**< Output sample rate (Hz). */
    uint32_t          out_channels;    /**< Number of output channels. */
    enum audio_endian out_endian;      /**< Output endianness. */

    bool swap_in;        /**< Whether to byte-swap input data. */
    bool swap_out;       /**< Whether to byte-swap output data. */
    bool needs_resample; /**< Whether resampling is required. */
    bool vad_enabled;    /**< Whether VAD is enabled for this session. */

    struct audio_resampler resampler;     /**< The resampler state. */
    struct vad_gru_state  *vad_state;     /**< The VAD GRU state. */
    float                  last_vad_prob; /**< The last computed voice probability [0.0 - 1.0]. */
};

/**
 * @brief Initializes a transcoding session from a configuration.
 *
 * @param session Pointer to the session to initialize.
 * @param config Pointer to the configuration block.
 * @param vad_arena Pointer to the memory arena used to allocate the VAD state (if enabled).
 * @return 0 on success, -1 on failure.
 */
// clang-format off
int transcode_session_init(
    struct transcode_session *restrict const      session,
    struct transcode_config const *restrict const config,
    struct audio_arena *restrict const            vad_arena);
// clang-format on

/**
 * @brief Processes an incoming audio packet through the transcoding pipeline.
 *
 * @param session Pointer to the active transcoding session.
 * @param in_data Pointer to the raw input audio payload.
 * @param in_len Length of the input audio payload in bytes.
 * @param out_arena Pointer to the scratch arena used for intermediate buffers and final output.
 * @param out_data Returns a pointer to the final transcoded payload in the arena.
 * @param out_len Returns the length of the final transcoded payload in bytes.
 * @return 0 on success, -1 on failure.
 */
// clang-format off
int transcode_process(
    struct transcode_session *restrict const session,
    uint8_t const *restrict const            in_data,
    size_t const                             in_len,
    struct audio_arena *restrict const       out_arena,
    uint8_t **restrict const                 out_data,
    size_t *restrict const                   out_len);
// clang-format on
