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
#include "orbit/server.h"

/**
 * @brief Initializes the WebSocket bridge, server socket, and connection pools.
 *
 * Warmups the WebSocket memory arenas (to avoid allocations during runtime),
 * opens and binds the non-blocking WebSocket server TCP socket, starts listening,
 * and submits the initial accept event to the server loop.
 *
 * @return 0 on success, or -1 on initialization failure.
 */
int ws_bridge_init(void);

/**
 * @brief Cleans up and closes the WebSocket server socket and destroys connection pools.
 */
void ws_bridge_cleanup(void);

/**
 * @brief Processes the completion of a socket accept event.
 *
 * Sets up non-blocking flags, allocates a session context, registers the new client
 * TCP socket for asynchronous recv operations, and re-submits the server socket accept event.
 *
 * @param ctx The IO event context representing the accept operation.
 * @param res The file descriptor of the accepted client socket, or a negative error code.
 */
void ws_bridge_process_accept(struct io_event_ctx *restrict const ctx, int const res);

/**
 * @brief Processes the completion of a WebSocket client recv operation.
 *
 * Performs the WebSocket upgrade handshake if not already handshaked, maps the socket
 * session to the corresponding SIP call session using the parsed internal ID, and delegates
 * message processing to wslay. Re-registers the socket for recv.
 *
 * @param ctx The IO event context representing the recv operation.
 * @param res The number of bytes received from the client socket, or a negative error/close code.
 */
void ws_bridge_process_recv(struct io_event_ctx *restrict const ctx, int const res);

/**
 * @brief Processes the completion of a WebSocket client send operation.
 *
 * Resumes sending queued wslay frames once the previous chunk finishes transmitting.
 *
 * @param ctx The IO event context representing the send operation.
 * @param res The number of bytes sent, or a negative error code.
 */
void ws_bridge_process_send(struct io_event_ctx *restrict const ctx, int const res);

/**
 * @brief Enqueues and sends a binary audio frame payload to the WebSocket client.
 *
 * Wraps the data inside a WebSocket binary frame and submits it for transmission.
 *
 * @param sip_call The associated SIP call session context containing the WebSocket session
 * connection.
 * @param data Pointer to the binary payload data buffer.
 * @param len Length of the binary data in bytes.
 */
void ws_bridge_send_binary(struct call_session *sip_call, uint8_t const *data, size_t len);

/**
 * @brief Closes the WebSocket connection associated with the call session.
 *
 * Performs a TCP socket shutdown to signal EOF to the client.
 *
 * @param sip_call The associated SIP call session context.
 */
void ws_bridge_close(struct call_session *sip_call);
