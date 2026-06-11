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
#include <netinet/in.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "orbit/rtp_jitter.h"

struct io_event_ctx;

#include "orbit/string_view.h"

// Explicit enumeration for required SIP methods
typedef enum sip_verb {
    SIP_VERB_UNKNOWN = 0,
    SIP_VERB_INVITE,
    SIP_VERB_ACK,
    SIP_VERB_CANCEL,
    SIP_VERB_BYE,
    SIP_VERB_OPTIONS,
    SIP_VERB_RESPONSE // For 2xx, 4xx, etc.
} sip_verb_t;

struct sdp_codec_info {
    uint32_t sample_rate;
    uint8_t  payload_type;
    uint8_t  channels;
};

// Extracted SDP properties
struct sdp_media_info {
    struct string_view    ip_addr;
    uint16_t              port;
    uint32_t              ptime;
    struct sdp_codec_info opus;
    struct sdp_codec_info pcmu;
    struct sdp_codec_info pcma;
    struct sdp_codec_info l16;
    struct sdp_codec_info dtmf;
};

// A parsed SIP header view
struct sip_header_view {
    struct string_view name;
    struct string_view value;
};

#define SIP_MAX_CUSTOM_HEADERS 16

// Represents a parsed SIP message (request or response)
struct sip_message {
    sip_verb_t             verb;
    struct string_view     call_id;
    struct string_view     from_tag;
    struct string_view     to_tag;
    struct string_view     cseq;
    struct string_view     via;
    struct string_view     body;
    struct sip_header_view custom_headers[SIP_MAX_CUSTOM_HEADERS];
    size_t                 custom_header_count;
    uint16_t               response_code; // If verb == SIP_VERB_RESPONSE
};

struct call_session {
    alignas(64) bool is_active;
    bool                  lock;
    int                   refcount;
    bool                  has_learned_remote_addr;
    int                   rtp_fd;
    uint16_t              tx_seq_num;
    uint32_t              tx_timestamp;
    uint32_t              tx_ssrc;
    void                 *ws_session;
    struct sockaddr_in    learned_remote_addr;
    struct sdp_media_info remote_sdp;
    struct sdp_media_info local_sdp;
    struct jitter_buffer  jitter;
    size_t                call_id_len;
    size_t                internal_id_len;
    char                  internal_id_buf[37];
    char                  call_id_buf[128];
};

// API
static inline void call_lock(struct call_session *call) {
    if (call == nullptr)
        return;
    while (__atomic_test_and_set(&call->lock, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__)
        __asm__ volatile("pause" ::: "memory");
#endif
    }
}

static inline void call_unlock(struct call_session *call) {
    if (call == nullptr)
        return;
    __atomic_clear(&call->lock, __ATOMIC_RELEASE);
}

void call_release(struct call_session *call);

int        sip_router_init(void);
void       sip_router_cleanup(void);
size_t     sip_router_active_call_count(void);
sip_verb_t sip_parse_verb(struct string_view const method);
bool sip_parse_message(struct string_view const raw, struct sip_message *restrict const out_msg);
bool sdp_parse(struct string_view const body, struct sdp_media_info *restrict const out_info);
struct call_session *sip_router_process(struct sip_message const *restrict const msg);

// Helper function to resolve RTP codec names for standard VoIP and WebRTC codecs
static inline char const *
rtp_get_codec_name(uint8_t const pt, struct sdp_media_info const *restrict const sdp) {
    if (sdp != nullptr) {
        if (sdp->opus.payload_type == pt)
            return "OPUS";
        if (sdp->pcmu.payload_type == pt)
            return "PCMU";
        if (sdp->pcma.payload_type == pt)
            return "PCMA";
        if (sdp->l16.payload_type == pt)
            return "L16";
        if (sdp->dtmf.payload_type == pt)
            return "telephone-event";
    }

    switch (pt) {
    case 0:
        return "PCMU";
    case 3:
        return "GSM";
    case 4:
        return "G723";
    case 5:
        return "DVI4 (8kHz)";
    case 6:
        return "DVI4 (16kHz)";
    case 7:
        return "LPC";
    case 8:
        return "PCMA";
    case 9:
        return "G722";
    case 10:
        return "L16 (Stereo)";
    case 11:
        return "L16 (Mono)";
    case 12:
        return "QCELP";
    case 13:
        return "CN";
    case 14:
        return "MPA";
    case 15:
        return "G728";
    case 16:
        return "DVI4 (11.025kHz)";
    case 17:
        return "DVI4 (22.050kHz)";
    case 18:
        return "G729";
    case 25:
        return "CelB";
    case 26:
        return "JPEG";
    case 31:
        return "H261";
    case 32:
        return "MPV";
    case 33:
        return "MP2T";
    case 34:
        return "H263";
    default:
        unreachable();
    }
}

// Find an existing call by Call-ID.
struct call_session *sip_router_find_call(struct string_view const call_id);

// Find an existing call by Internal ID.
struct call_session *sip_router_find_call_by_internal_id(struct string_view const internal_id);

// Generates the 200 OK SDP body into a zero-allocation buffer.
// Returns the length of the written string, or 0 if buffer is too small.
size_t sdp_generate_reply(
    struct call_session const *restrict const session,
    char *restrict const buffer,
    size_t const max_len);

size_t sdp_generate_capabilities_reply(char *restrict const buffer, size_t const max_len);

// Generates a SIP response (e.g., "SIP/2.0 200 OK") based on the incoming request.
// Injects the provided SDP body if sdp_body.length > 0.
size_t sip_generate_response(
    struct sip_message const *restrict const req,
    int const status_code,
    char const *restrict const reason_phrase,
    struct string_view const sdp_body,
    char *restrict const buffer,
    size_t const max_len);

void sip_router_process_recv(struct io_event_ctx *restrict const ctx, size_t const len);
void sip_router_process_send(struct io_event_ctx *restrict const ctx);
