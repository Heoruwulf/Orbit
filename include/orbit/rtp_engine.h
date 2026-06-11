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
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct call_session; // Forward declaration
struct io_event_ctx;

/**
 * @brief Initializes the RTP engine thread-local memory pools.
 *
 * Creates the RTP buffer and context pools based on the configured maximum calls.
 *
 * @return 0 on success, or -1 on memory pool allocation failure.
 */
int rtp_engine_init(void);

/**
 * @brief Cleans up and destroys the RTP engine thread-local memory pools.
 */
void rtp_engine_cleanup(void);

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
 * @brief Processes an incoming RTP packet received via io_uring.
 *
 * Decodes/validates the RTP header, routes the payload to the WS bridge,
 * learns/updates the remote peer address if needed, and re-submits the receive event.
 *
 * @param ctx The IO event context representing the read operation.
 * @param bytes_read The total number of bytes read from the socket.
 */
void rtp_engine_process_packet(struct io_event_ctx *restrict const ctx, size_t const bytes_read);

/**
 * @brief Handles the completion of an asynchronous RTP packet send operation.
 *
 * Frees the associated IO event context.
 *
 * @param ctx The IO event context representing the send operation.
 */
void rtp_engine_process_send_complete(struct io_event_ctx *restrict const ctx);

/**
 * @brief Releases the memory allocated for an IO event context back to the pool.
 *
 * @param ctx The IO event context to be freed.
 */
void rtp_engine_free_ctx(struct io_event_ctx *restrict const ctx);

/**
 * @brief Generates an RTP packet containing comfort noise/silence payload.
 *
 * Constructs a standard RTP packet header and fills the payload with the appropriate
 * silence byte (e.g., PCMU, PCMA, L16) depending on the negotiated codecs.
 *
 * @param session The call session context.
 * @param buffer Output buffer where the RTP packet will be constructed.
 * @param out_length Pointer to store the written length of the packet.
 * @return true on success, or false if no supported codec is negotiated.
 */
bool rtp_generate_silence(
    struct call_session *restrict const session,
    void *restrict const buffer,
    size_t *restrict const out_length);

/**
 * @brief Asynchronously transmits a comfort noise/silence RTP packet.
 *
 * Allocates resources, generates a comfort noise payload, and submits a sendmsg
 * event to io_uring for the remote peer.
 *
 * @param session The call session context.
 * @param fd The UDP socket file descriptor.
 * @return true on success, or false on allocation/generation failure.
 */
bool rtp_engine_send_silence(struct call_session *restrict const session, int const fd);

/**
 * @brief Generates an RFC 2833 / RFC 4733 telephony event (DTMF) RTP packet.
 *
 * Constructs the RTP header and the DTMF payload (digit, volume, duration, is_end, etc.).
 *
 * @param session The call session context.
 * @param digit The DTMF digit character representation/index (0-15).
 * @param is_start True if this packet starts the DTMF tone.
 * @param is_end True if this is the final packet ending the DTMF tone.
 * @param duration Duration of the DTMF tone in timestamp units.
 * @param buffer Output buffer to write the RTP packet.
 * @param out_length Pointer to store the written length of the packet.
 * @return true on success, or false if DTMF was not negotiated.
 */
bool rtp_generate_dtmf(
    struct call_session *restrict const session,
    uint8_t const  digit,
    bool const     is_start,
    bool const     is_end,
    uint16_t const duration,
    void *restrict const buffer,
    size_t *restrict const out_length);

/**
 * @brief Asynchronously transmits a DTMF telephony event RTP packet.
 *
 * Allocates resources, generates the DTMF payload, and submits a sendmsg
 * event to io_uring for the remote peer.
 *
 * @param session The call session context.
 * @param fd The UDP socket file descriptor.
 * @param digit The DTMF digit representation (0-15).
 * @param is_start True if this packet starts the DTMF tone.
 * @param is_end True if this is the final packet ending the DTMF tone.
 * @param duration Duration of the DTMF tone in timestamp units.
 * @return true on success, or false on allocation/generation failure.
 */
bool rtp_engine_send_dtmf(
    struct call_session *restrict const session,
    int const      fd,
    uint8_t const  digit,
    bool const     is_start,
    bool const     is_end,
    uint16_t const duration);

/**
 * @brief Asynchronously transmits a raw audio payload wrapped in an RTP header.
 *
 * Prepend standard RTP header prefix, copies the payload, and submits a sendmsg
 * event to io_uring.
 *
 * @param session The call session context.
 * @param fd The UDP socket file descriptor.
 * @param payload The raw audio payload bytes.
 * @param payload_len Length of the payload in bytes.
 * @return true on success, or false on failure.
 */
bool rtp_engine_send_payload(
    struct call_session *restrict const session,
    int const fd,
    uint8_t const *restrict const payload,
    size_t const payload_len);

/**
 * @brief Allocates a new UDP port for RTP and submits a non-blocking recvmsg.
 *
 * Searches for a free UDP port in the configured min-max range, binds to it,
 * allocates event/buffer contexts, and registers the descriptor for io_uring read.
 *
 * @param session The call session context.
 * @param out_port Pointer to store the successfully bound UDP port.
 * @return The bound socket file descriptor on success, or -1 on failure.
 */
int rtp_engine_allocate_port(
    struct call_session *restrict const session,
    uint16_t *restrict const out_port);

/**
 * @brief Closes the socket and releases the associated RTP UDP port.
 *
 * @param fd The UDP socket file descriptor to close.
 * @param port The UDP port to free.
 */
void rtp_engine_free_port(int const fd, uint16_t const port);
