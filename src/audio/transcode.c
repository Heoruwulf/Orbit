#include <stdalign.h>
#include <string.h>
#include "g711_tables.h"
#include "orbit/audio.h"
#include "orbit/macros.h"

// G.711 Encoders
static inline uint8_t linear_to_ulaw(int16_t pcm_val) {
    int16_t mask;
    int16_t seg;
    uint8_t uval;

    if (pcm_val < 0) {
        pcm_val = (int16_t)-pcm_val;
        mask    = 0x7F;
    } else {
        mask = 0xFF;
    }
    if (pcm_val > 32635) {
        pcm_val = 32635;
    }
    pcm_val = (int16_t)(pcm_val + 128);

    if (pcm_val < 256)
        seg = 0;
    else if (pcm_val < 512)
        seg = 1;
    else if (pcm_val < 1024)
        seg = 2;
    else if (pcm_val < 2048)
        seg = 3;
    else if (pcm_val < 4096)
        seg = 4;
    else if (pcm_val < 8192)
        seg = 5;
    else if (pcm_val < 16384)
        seg = 6;
    else
        seg = 7;

    uval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 3)) & 0xF));
    return (uint8_t)(uval ^ mask);
}

static inline uint8_t linear_to_alaw(int16_t pcm_val) {
    int16_t mask;
    int16_t seg;
    uint8_t aval;

    if (pcm_val >= 0) {
        mask = 0xD5;
    } else {
        mask    = 0x55;
        pcm_val = (int16_t)(-pcm_val - 1);
    }

    if (pcm_val < 0) {
        pcm_val = 0;
    }

    if (pcm_val < 256)
        seg = 0;
    else if (pcm_val < 512)
        seg = 1;
    else if (pcm_val < 1024)
        seg = 2;
    else if (pcm_val < 2048)
        seg = 3;
    else if (pcm_val < 4096)
        seg = 4;
    else if (pcm_val < 8192)
        seg = 5;
    else if (pcm_val < 16384)
        seg = 6;
    else
        seg = 7;

    if (seg == 0) {
        aval = (uint8_t)((pcm_val >> 4) & 0x0F);
    } else {
        aval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 3)) & 0x0F));
    }

    return (uint8_t)(aval ^ mask);
}

static inline bool is_host_little_endian(void) {
    uint16_t const x = 0x01;
    return *((uint8_t const *)&x) == 1;
}

int transcode_session_init(
    struct transcode_session *const      session,
    struct transcode_config const *const config,
    struct audio_arena *const            vad_arena) {

    if (unlikely(session == nullptr || config == nullptr)) {
        return -1;
    }

    __builtin_memset(session, 0, sizeof(struct transcode_session));

    session->in_codec       = config->in_codec;
    session->in_channels    = config->in_channels;
    session->in_endian      = config->in_endian;
    session->in_sample_rate = config->in_sample_rate;

    session->out_codec       = config->out_codec;
    session->out_channels    = config->out_channels;
    session->out_endian      = config->out_endian;
    session->out_sample_rate = config->out_sample_rate;

    session->vad_enabled   = config->vad_enabled;
    session->vad_state     = nullptr;
    session->last_vad_prob = session->vad_enabled ? 0.0F : -1.0F;

    bool const host_le = is_host_little_endian();

    if (session->in_codec == CODEC_L16) {
        if ((session->in_endian == ENDIAN_LITTLE && !host_le) ||
            (session->in_endian == ENDIAN_BIG && host_le))
        {
            session->swap_in = true;
        }
    }

    if (session->out_codec == CODEC_L16) {
        if ((session->out_endian == ENDIAN_LITTLE && !host_le) ||
            (session->out_endian == ENDIAN_BIG && host_le))
        {
            session->swap_out = true;
        }
    }

    if (session->vad_enabled && vad_arena != nullptr) {
        session->vad_state = vad_gru_init(vad_arena);
        if (session->vad_state == nullptr) {
            return -1;
        }
    }

    session->needs_resample = (session->in_sample_rate != session->out_sample_rate);
    if (session->needs_resample) {
        if (resampler_init(
                &session->resampler,
                session->in_sample_rate,
                session->out_sample_rate) != 0)
        {
            return -1;
        }
    }

    return 0;
}

static void swap_endian_l16(int16_t *restrict const data, size_t const samples) {
    for (size_t i = 0; i < samples; ++i) {
        uint16_t const val = (uint16_t)data[i];
        data[i]            = (int16_t)((val << 8) | (val >> 8));
    }
}

int transcode_process(
    struct transcode_session *const session,
    uint8_t const *const            in_data,
    size_t const                    in_len,
    struct audio_arena *const       out_arena,
    uint8_t **const                 out_data_ptr,
    size_t *const                   out_len_ptr) {

    if (unlikely(
            session == nullptr || in_data == nullptr || out_arena == nullptr ||
            out_data_ptr == nullptr || out_len_ptr == nullptr))
    {
        return -1;
    }

    if (in_len == 0) {
        *out_data_ptr = nullptr;
        *out_len_ptr  = 0;
        return 0;
    }

    if (session->in_codec == CODEC_PASS && session->out_codec == CODEC_PASS) {
        uint8_t *const raw_buf = arena_alloc(out_arena, in_len);
        if (!raw_buf)
            return -1;
        __builtin_memcpy(raw_buf, in_data, in_len);
        *out_data_ptr = raw_buf;
        *out_len_ptr  = in_len;
        return 0;
    }

    int16_t *decode_buf     = nullptr;
    size_t   decode_samples = 0;

    if (session->in_codec == CODEC_PCMU) {
        decode_samples = in_len;
        decode_buf     = arena_alloc(out_arena, decode_samples * sizeof(int16_t));
        if (!decode_buf)
            return -1;
        for (size_t i = 0; i < decode_samples; ++i) {
            decode_buf[i] = PCMU_TO_L16_LUT[in_data[i]];
        }
    } else if (session->in_codec == CODEC_PCMA) {
        decode_samples = in_len;
        decode_buf     = arena_alloc(out_arena, decode_samples * sizeof(int16_t));
        if (!decode_buf)
            return -1;
        for (size_t i = 0; i < decode_samples; ++i) {
            decode_buf[i] = PCMA_TO_L16_LUT[in_data[i]];
        }
    } else if (session->in_codec == CODEC_L16) {
        decode_samples = in_len / (sizeof(int16_t) * session->in_channels);
        decode_buf     = arena_alloc(out_arena, in_len);
        if (!decode_buf)
            return -1;
        __builtin_memcpy(decode_buf, in_data, in_len);
        if (session->swap_in) {
            swap_endian_l16(decode_buf, decode_samples * session->in_channels);
        }
    } else {
        return -1;
    }

    session->last_vad_prob = -1.0f;
    if (session->vad_enabled && session->vad_state != nullptr &&
        session->in_sample_rate == VAD_SAMPLE_RATE)
    {
        session->last_vad_prob = vad_gru_process_pcm(
            session->vad_state,
            decode_buf,
            decode_samples,
            VAD_SAMPLE_RATE,
            (int)session->in_channels);
    }

    int16_t *resampled_buf     = decode_buf;
    size_t   resampled_samples = decode_samples;

    if (session->needs_resample) {
        size_t const out_cap =
            (decode_samples * session->out_sample_rate / session->in_sample_rate) + 128;
        resampled_buf = arena_alloc(out_arena, out_cap * session->out_channels * sizeof(int16_t));
        if (!resampled_buf)
            return -1;
        resampled_samples = resample_l16_advanced(
            &session->resampler,
            session->in_sample_rate,
            session->out_sample_rate,
            decode_buf,
            decode_samples,
            resampled_buf,
            out_cap);
    }

    if (session->vad_enabled && session->vad_state != nullptr && session->last_vad_prob == -1.0f &&
        session->out_sample_rate == VAD_SAMPLE_RATE)
    {
        session->last_vad_prob = vad_gru_process_pcm(
            session->vad_state,
            resampled_buf,
            resampled_samples,
            VAD_SAMPLE_RATE,
            (int)session->out_channels);
    }

    uint8_t *final_out = nullptr;
    size_t   final_len = 0;

    if (session->out_codec == CODEC_PCMU) {
        final_len = resampled_samples;
        final_out = arena_alloc(out_arena, final_len);
        if (!final_out)
            return -1;
        for (size_t i = 0; i < resampled_samples; ++i) {
            final_out[i] = linear_to_ulaw(resampled_buf[i]);
        }
    } else if (session->out_codec == CODEC_PCMA) {
        final_len = resampled_samples;
        final_out = arena_alloc(out_arena, final_len);
        if (!final_out)
            return -1;
        for (size_t i = 0; i < resampled_samples; ++i) {
            final_out[i] = linear_to_alaw(resampled_buf[i]);
        }
    } else if (session->out_codec == CODEC_L16) {
        final_len = resampled_samples * session->out_channels * sizeof(int16_t);
        if (session->swap_out) {
            swap_endian_l16(resampled_buf, resampled_samples * session->out_channels);
        }
        final_out = arena_alloc(out_arena, final_len);
        if (!final_out)
            return -1;
        __builtin_memcpy(final_out, resampled_buf, final_len);
    } else {
        return -1;
    }

    *out_data_ptr = final_out;
    *out_len_ptr  = final_len;

    return 0;
}
