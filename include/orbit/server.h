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
#include <stddef.h>
#include <sys/socket.h>
#include <sys/uio.h>

struct io_uring;
struct call_session;

/**
 * @brief Enumerates the type of completed I/O events managed by the server loop.
 */
typedef enum event_type {
    EVENT_TYPE_SIGNAL    = 1, /**< Intercepted signal (SIGINT/SIGTERM) notification event. */
    EVENT_TYPE_RTP_RECV  = 2, /**< Completion of an RTP packet read operation. */
    EVENT_TYPE_RTP_SEND  = 3, /**< Completion of an RTP packet write operation. */
    EVENT_TYPE_WS_ACCEPT = 4, /**< Completion of a WebSocket server socket accept operation. */
    EVENT_TYPE_WS_RECV   = 5, /**< Completion of a WebSocket frame read operation. */
    EVENT_TYPE_WS_SEND   = 6, /**< Completion of a WebSocket frame write operation. */
    EVENT_TYPE_SIP_RECV  = 7, /**< Completion of a SIP message read operation. */
    EVENT_TYPE_SIP_SEND  = 8  /**< Completion of a SIP message write operation. */
} event_type_t;

/**
 * @brief User data context associated with each io_uring asynchronous I/O request.
 *
 * Keeps track of target descriptors, associated call sessions, scatter-gather I/O vectors,
 * and endpoint addresses for processing event completions.
 */
struct io_event_ctx {
    event_type_t type;           /**< Type of the submitted asynchronous I/O operation. */
    int          fd;             /**< Socket or signal file descriptor associated with the event. */
    void *restrict buffer;       /**< Pointer to the primary data payload buffer. */
    size_t               length; /**< Size of the data payload buffer or written data size. */
    struct call_session *session; /**< Associated call session context (or WebSocket session). */
    struct msghdr        msg;     /**< Message header for scatter/gather socket operations. */
    struct iovec       iov; /**< Single-element scatter/gather I/O vector referencing the buffer. */
    struct sockaddr_in remote_addr; /**< Source or destination IP socket address. */
};

/**
 * @brief Initializes the thread-local io_uring queue.
 *
 * @return 0 on success, or -1 on initialization failure.
 */
int server_init(void);

/**
 * @brief Waits for a single completion queue event (CQE) and processes it.
 *
 * Blocks until a completed I/O request is available, invokes the appropriate
 * component-specific handler based on the event type, and marks the CQE as seen.
 */
void server_poll_once(void);

/**
 * @brief Runs the main server event loop for the current thread.
 *
 * Submits the supervisor shutdown event descriptor to the io_uring, and polls
 * events until stopped or the server finishes draining active calls.
 *
 * @param event_fd File descriptor used to signal shutdown or thread interruption.
 */
void server_run(int event_fd);

/**
 * @brief Signals the server thread loop to stop running.
 */
void server_stop(void);

/**
 * @brief Cleans up and exits the thread-local io_uring queue.
 */
void server_cleanup(void);

/**
 * @brief Checks if the server is currently in a draining state.
 *
 * During draining, new calls are refused, and the server shuts down once active
 * calls reach zero.
 *
 * @return true if the server is draining, or false otherwise.
 */
bool server_is_draining(void);

/**
 * @brief Submits an asynchronous recvmsg operation to io_uring.
 *
 * @param ctx The IO event context representing the read operation.
 * @return Number of submitted SQEs on success, or -1 on failure.
 */
int server_submit_recv(struct io_event_ctx *restrict const ctx);

/**
 * @brief Submits an asynchronous sendmsg operation to io_uring.
 *
 * @param ctx The IO event context representing the write/sendmsg operation.
 * @return Number of submitted SQEs on success, or -1 on failure.
 */
int server_submit_sendmsg(struct io_event_ctx *restrict const ctx);

/**
 * @brief Submits an asynchronous socket accept operation to io_uring.
 *
 * @param ctx The IO event context representing the accept operation.
 * @return Number of submitted SQEs on success, or -1 on failure.
 */
int server_submit_accept(struct io_event_ctx *restrict const ctx);

/**
 * @brief Submits an asynchronous raw socket send operation to io_uring.
 *
 * @param ctx The IO event context representing the send operation.
 * @return Number of submitted SQEs on success, or -1 on failure.
 */
int server_submit_send(struct io_event_ctx *restrict const ctx);
