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

// API
int  rtp_engine_init(void);
void rtp_engine_cleanup(void);

// Generates the 200 OK SDP body into a zero-allocation buffer.
// Returns the length of the written string, or 0 if buffer is too small.
size_t sdp_generate_reply(
    struct call_session const *restrict const session,
    char *restrict const buffer,
    size_t const max_len);

// Processes a completed io_uring read of an RTP packet
void rtp_engine_process_packet(struct io_event_ctx *restrict const ctx, size_t const bytes_read);

void rtp_engine_process_send_complete(struct io_event_ctx *restrict const ctx);
void rtp_engine_free_ctx(struct io_event_ctx *restrict const ctx);

bool rtp_generate_silence(
    struct call_session *restrict const session,
    void *restrict const buffer,
    size_t *restrict const out_length);

bool rtp_engine_send_silence(struct call_session *restrict const session, int const fd);

bool rtp_generate_dtmf(
    struct call_session *restrict const session,
    uint8_t const  digit,
    bool const     is_start,
    bool const     is_end,
    uint16_t const duration,
    void *restrict const buffer,
    size_t *restrict const out_length);

bool rtp_engine_send_dtmf(
    struct call_session *restrict const session,
    int const      fd,
    uint8_t const  digit,
    bool const     is_start,
    bool const     is_end,
    uint16_t const duration);

bool rtp_engine_send_payload(
    struct call_session *restrict const session,
    int const fd,
    uint8_t const *restrict const payload,
    size_t const payload_len);

// Returns a bound UDP socket File Descriptor, and writes the bound port to `out_port`.
// Returns -1 if no ports are available or binding fails.
int rtp_engine_allocate_port(
    struct call_session *restrict const session,
    uint16_t *restrict const out_port);

// Releases the port back to the free list and closes the UDP socket.
void rtp_engine_free_port(int const fd, uint16_t const port);
