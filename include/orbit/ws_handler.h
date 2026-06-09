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

struct ws_connection {
    wslay_event_context_ptr ctx;
    int                     fd;
};

// Initializes a wslay connection context.
// Returns true on success.
bool ws_connection_init(
    struct ws_connection               *conn,
    int                                 fd,
    struct wslay_event_callbacks const *callbacks,
    void                               *user_data);

// Frees the wslay connection context.
void ws_connection_free(struct ws_connection *conn);

// Parses the HTTP GET request to extract `uuid` and `callid`.
// Returns true if valid and outputs them.
bool ws_parse_handshake_url(
    struct string_view const req_line,
    struct string_view *restrict const out_internal_id);

// Parses a WebSocket text frame containing JSON: {"type":"dtmf","digit":"1"}.
// Returns true and writes the integer representation (0-15) to out_digit.
bool ws_parse_dtmf_json(struct string_view const text, uint8_t *restrict const out_digit);

// Generates the initial JSON metadata payload string.
// Returns the length written to buffer.
size_t ws_generate_metadata_json(
    struct call_session const *restrict const session,
    char *restrict const buffer,
    size_t const max_len);
