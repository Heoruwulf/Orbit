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
#include <wslay/wslay.h>
#include "orbit/sip_router.h"

/**
 * @brief Represents a WebSocket connection wrapping a wslay context.
 */
struct ws_connection {
    wslay_event_context_ptr ctx; /**< The wslay event context pointer. */
    int                     fd;  /**< Client TCP socket file descriptor. */
};

/**
 * @brief Initializes a wslay event connection context.
 *
 * Wraps a socket descriptor and registers wslay event callbacks for read/write
 * operations on a WebSocket client session.
 *
 * @param conn The connection structure to initialize.
 * @param fd The TCP socket file descriptor.
 * @param callbacks The callback table containing I/O callbacks.
 * @param user_data Arbitrary data passed to connection callback functions.
 * @return true on success, or false on wslay initialization failure.
 */
bool ws_connection_init(
    struct ws_connection               *conn,
    int                                 fd,
    struct wslay_event_callbacks const *callbacks,
    void                               *user_data);

/**
 * @brief Frees the wslay event connection context.
 *
 * @param conn The connection structure to free.
 */
void ws_connection_free(struct ws_connection *conn);

/**
 * @brief Parses the HTTP GET upgrade request line to extract the internal call UUID.
 *
 * Locates the query string id value parameter in a URI like "GET /media?id=<uuid> HTTP/1.1".
 *
 * @param req_line The raw HTTP request start line.
 * @param out_internal_id Output string view populated with the extracted ID substring.
 * @return true if successfully parsed, or false otherwise.
 */
bool ws_parse_handshake_url(
    struct string_view const req_line,
    struct string_view *restrict const out_internal_id);

/**
 * @brief Parses a DTMF digit transaction request formatted in JSON.
 *
 * Expects a WebSocket text frame in the format: {"type":"dtmf","digit":"1"}.
 * Converts characters (0-9, *, #, A-D) into their standard telephony indices (0-15).
 *
 * @param text The WebSocket text frame payload string.
 * @param out_digit Output pointer to write the parsed DTMF digit index (0-15).
 * @return true if successfully parsed, or false otherwise.
 */
bool ws_parse_dtmf_json(struct string_view const text, uint8_t *restrict const out_digit);

/**
 * @brief Generates the initial JSON metadata payload describing the negotiated call properties.
 *
 * Serializes the session metadata parameters (sample rate, channels, negotiated codec, ptime,
 * SIP Call-ID) into a JSON document using a zero-allocation heap buffer.
 *
 * @param session The call session structure.
 * @param buffer Output buffer where the formatted metadata JSON will be written.
 * @param max_len Maximum writable length of the output buffer.
 * @return Length of the formatted JSON string, or 0 if writing failed or size exceeded max_len.
 */
size_t ws_generate_metadata_json(
    struct call_session const *restrict const session,
    char *restrict const buffer,
    size_t const max_len);

