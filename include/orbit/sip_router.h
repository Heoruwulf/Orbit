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

#ifndef unreachable
#define unreachable() __builtin_unreachable()
#endif
#include "orbit/audio.h"
#include "orbit/rtp_jitter.h"

struct io_event_ctx;

#include "orbit/string_view.h"

/**
 * @brief Enumeration of supported SIP request methods and response type.
 */
typedef enum sip_verb {
    SIP_VERB_UNKNOWN = 0, /**< Unknown or unsupported SIP method. */
    SIP_VERB_INVITE,      /**< INVITE request used to initiate a call. */
    SIP_VERB_ACK,         /**< ACK request acknowledging a final response. */
    SIP_VERB_CANCEL,      /**< CANCEL request terminating a pending call. */
    SIP_VERB_BYE,         /**< BYE request terminating an active session. */
    SIP_VERB_OPTIONS,     /**< OPTIONS query for capability exchange or pings. */
    SIP_VERB_RESPONSE     /**< Message is a SIP response status. */
} sip_verb_t;

/**
 * @brief SDP parameters for a single media codec.
 */
struct sdp_codec_info {
    uint32_t sample_rate;  /**< Sample rate in Hz (e.g., 8000, 48000). */
    uint8_t  payload_type; /**< RTP payload type index, or 255 if not negotiated. */
    uint8_t  channels;     /**< Number of audio channels (e.g. 1 for mono, 2 for stereo). */
};

/**
 * @brief Extracted SDP media block containing connection and codec properties.
 */
struct sdp_media_info {
    struct string_view    ip_addr; /**< Connection IP address from the c= line. */
    uint16_t              port;    /**< Media transport port from the m= line. */
    uint32_t              ptime;   /**< Packetization time interval in ms. */
    struct sdp_codec_info opus;    /**< Negotiated properties for the Opus codec. */
    struct sdp_codec_info pcmu;    /**< Negotiated properties for the PCMU/G.711u codec. */
    struct sdp_codec_info pcma;    /**< Negotiated properties for the PCMA/G.711a codec. */
    struct sdp_codec_info l16;     /**< Negotiated properties for the L16 linear PCM codec. */
    struct sdp_codec_info dtmf;    /**< Negotiated properties for telephone-event DTMF packets. */
};

/**
 * @brief Represents a single parsed SIP header key-value slice.
 */
struct sip_header_view {
    struct string_view name;  /**< Header key name string view. */
    struct string_view value; /**< Header value string view. */
};

#define SIP_MAX_CUSTOM_HEADERS 16

/**
 * @brief Represents a parsed incoming SIP request or response message.
 */
struct sip_message {
    sip_verb_t         verb;     /**< The parsed SIP method/verb. */
    struct string_view call_id;  /**< Unique Call-ID header slice. */
    struct string_view from_tag; /**< Initiating From header tag slice. */
    struct string_view to_tag;   /**< Recipient To header tag slice. */
    struct string_view cseq;     /**< Command sequence index string view. */
    struct string_view via;      /**< Routing path Via header slice. */
    struct string_view body;     /**< Pointer and length of the message body (SDP payload). */
    struct sip_header_view
             custom_headers[SIP_MAX_CUSTOM_HEADERS]; /**< Array of parsed custom X- headers. */
    size_t   custom_header_count; /**< Number of custom headers successfully parsed. */
    uint16_t response_code;       /**< SIP response code, populated if verb == SIP_VERB_RESPONSE. */
};

/**
 * @brief Represents an active media bridge call session.
 *
 * Stores RTP tracking info, negotiated SDP capabilities, jitter buffers, and WebSocket linkages.
 * Cacheline-aligned (alignas(64)) to prevent false sharing in multi-core processing loops.
 */
struct call_session {
    alignas(64) bool is_active;    /**< Session state flag. */
    bool lock;                     /**< Atomic spinlock flag protecting this session. */
    int  refcount;                 /**< Atomic reference count for lock-free session reuse. */
    bool has_learned_remote_addr;  /**< True if the remote RTP sender endpoint has been resolved. */
    int  rtp_fd;                   /**< UDP socket descriptor for RTP traffic. */
    uint16_t           tx_seq_num; /**< Monotonically increasing RTP transmitter sequence number. */
    uint32_t           tx_timestamp; /**< RTP transmitter timestamp tracker. */
    uint32_t           tx_ssrc;      /**< Synchronization source identifier for outbound RTP. */
    void              *ws_session;   /**< Associated WebSocket session bridge pointer. */
    struct sockaddr_in learned_remote_addr; /**< Resolved IP address/port of the remote RTP peer. */
    struct sdp_media_info
        remote_sdp; /**< SDP connection and codec capabilities parsed from remote peer. */
    struct sdp_media_info local_sdp;       /**< SDP capabilities offered locally. */
    struct jitter_buffer  jitter;          /**< Jitter buffer tracking inbound RTP packets. */
    size_t                call_id_len;     /**< Length of the cached Call-ID string. */
    size_t                internal_id_len; /**< Length of the internal UUID string. */
    char internal_id_buf[37]; /**< Cached buffer containing the internal UUID string. */
    char call_id_buf[128];    /**< Cached buffer containing the Call-ID string. */

    struct transcode_session ts_to_ws;
    struct transcode_session ts_to_rtp;
    bool                     transcode_initialized;
    alignas(32) uint8_t vad_arena_buf[8192];
    struct audio_arena vad_arena;
    alignas(32) uint8_t rtp_scratch_buf[8192];
    struct audio_arena rtp_scratch;
    alignas(32) uint8_t ws_scratch_buf[8192];
    struct audio_arena ws_scratch;
};

/**
 * @brief Acquires an atomic spinlock on a call session.
 *
 * Blocks using CPU pause hints until the session lock is acquired.
 *
 * @param call The call session to lock.
 */
static inline void call_lock(struct call_session *call) {
    if (call == nullptr)
        return;
    while (__atomic_test_and_set(&call->lock, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__)
        __asm__ volatile("pause" ::: "memory");
#endif
    }
}

/**
 * @brief Releases the atomic spinlock on a call session.
 *
 * @param call The call session to unlock.
 */
static inline void call_unlock(struct call_session *call) {
    if (call == nullptr)
        return;
    __atomic_clear(&call->lock, __ATOMIC_RELEASE);
}

/**
 * @brief Decrements the call session reference count and releases it if zero.
 *
 * If the reference count drops to zero, the call session resources are freed back
 * to the call pool.
 *
 * @param call The call session pointer to release.
 */
void call_release(struct call_session *call);

/**
 * @brief Initializes the SIP router structure, socket, and memory pools.
 *
 * Allocates the active call array, creates the call memory pool, opens and binds
 * the SIP UDP socket, and submits an initial recvmsg to the server loop.
 *
 * @return 0 on success, or -1 on failure.
 */
int sip_router_init(void);

/**
 * @brief Cleans up and frees all SIP router resources and socket.
 */
void sip_router_cleanup(void);

/**
 * @brief Counts the total number of active call sessions.
 *
 * @return The current number of active calls.
 */
size_t sip_router_active_call_count(void);

/**
 * @brief Parses the SIP verb/method from a method string view.
 *
 * Matches strings such as "INVITE", "ACK", "CANCEL", "BYE", "OPTIONS", or "SIP/2.0".
 *
 * @param method The method string view.
 * @return The corresponding sip_verb_t enumeration value.
 */
sip_verb_t sip_parse_verb(struct string_view const method);

/**
 * @brief Parses a raw SIP message string.
 *
 * Extracts the start line verb, parses all header lines (including Call-ID, From,
 * To, CSeq, Via, and X- custom headers), and locates the body segment.
 *
 * @param raw The raw SIP message data string view.
 * @param out_msg Pointer to the sip_message structure to populate.
 * @return true if parsing succeeds, or false otherwise.
 */
bool sip_parse_message(struct string_view const raw, struct sip_message *restrict const out_msg);

/**
 * @brief Parses a raw SDP body string view to extract media information.
 *
 * Extracts the media connection IP address, media port, codec payload mappings
 * (for Opus, PCMU, PCMA, L16, DTMF telephone-events), channels, sample rates, and ptime.
 *
 * @param body The raw SDP text string view.
 * @param out_info Pointer to the sdp_media_info structure to populate.
 * @return true if a valid connection IP and port are negotiated, or false otherwise.
 */
bool sdp_parse(struct string_view const body, struct sdp_media_info *restrict const out_info);

/**
 * @brief Processes an incoming parsed SIP message within the call routing logic.
 *
 * Maps messages to existing sessions or spawns new call_session instances
 * (on INVITE). Handles session destruction on BYE or CANCEL.
 *
 * @param msg The parsed SIP message context.
 * @return The associated active call session pointer, or nullptr if none/destroyed.
 */
struct call_session *sip_router_process(struct sip_message const *restrict const msg);

/**
 * @brief Helper function to resolve the human-readable codec name.
 *
 * Matches the payload type against standard and SDP-negotiated audio codecs.
 *
 * @param pt The payload type index.
 * @param sdp Pointer to the session SDP media info context.
 * @return A static string pointer representing the codec name.
 */
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

/**
 * @brief Finds an active call session by its external SIP Call-ID.
 *
 * Increments the reference count of the returned call session if found.
 *
 * @param call_id The SIP Call-ID string view.
 * @return The call session pointer, or nullptr if not found.
 */
struct call_session *sip_router_find_call(struct string_view const call_id);

/**
 * @brief Finds an active call session by its internal UUID string view.
 *
 * Increments the reference count of the returned call session if found.
 *
 * @param internal_id The internal UUID string view.
 * @return The call session pointer, or nullptr if not found.
 */
struct call_session *sip_router_find_call_by_internal_id(struct string_view const internal_id);

/**
 * @brief Generates the 200 OK SDP answer body.
 *
 * Writes the negotiated media attributes (codecs, ports) based on the session details
 * into a zero-allocation buffer.
 *
 * @param session The call session context containing negotiated ports.
 * @param buffer Output buffer where the SDP string will be written.
 * @param max_len Maximum writable length of the output buffer.
 * @return Length of the written string, or 0 if the buffer is too small.
 */
size_t sdp_generate_reply(
    struct call_session const *restrict const session,
    char *restrict const buffer,
    size_t const max_len);

/**
 * @brief Generates a generic SDP capabilities reply string.
 *
 * Writes a default list of supported codecs to the output buffer.
 *
 * @param buffer Output buffer where the capabilities SDP will be written.
 * @param max_len Maximum writable length of the output buffer.
 * @return Length of the written string, or 0 if the buffer is too small.
 */
size_t sdp_generate_capabilities_reply(char *restrict const buffer, size_t const max_len);

/**
 * @brief Generates a standard SIP response string.
 *
 * Formats headers based on the original request context and appends the SDP body.
 *
 * @param req The original SIP request message context.
 * @param status_code The SIP response status code (e.g., 200, 486, 503).
 * @param reason_phrase The text description of the status code.
 * @param sdp_body The optional SDP body to append.
 * @param buffer Output buffer where the SIP response will be formatted.
 * @param max_len Maximum writable length of the output buffer.
 * @return Length of the formatted SIP response, or 0 if the buffer is too small.
 */
size_t sip_generate_response(
    struct sip_message const *restrict const req,
    int const status_code,
    char const *restrict const reason_phrase,
    struct string_view const sdp_body,
    char *restrict const buffer,
    size_t const max_len);

/**
 * @brief Processes a completed SIP packet receive operation.
 *
 * Parses the incoming SIP packet, processes it, formats/submits a response
 * (e.g., to INVITE, OPTIONS, BYE) via io_uring, and re-submits a recv event.
 *
 * @param ctx The IO event context representing the read operation.
 * @param len The size of the received packet payload in bytes.
 */
void sip_router_process_recv(struct io_event_ctx *restrict const ctx, size_t const len);

/**
 * @brief Handles the completion of an asynchronous SIP send operation.
 *
 * Currently a no-op handler.
 *
 * @param ctx The IO event context representing the send operation.
 */
void sip_router_process_send(struct io_event_ctx *restrict const ctx);
