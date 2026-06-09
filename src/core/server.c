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
#include "orbit/server.h"
#include <liburing.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <stdint.h>
#include <threads.h>

static thread_local struct io_uring     ring       = {};
static thread_local int                 m_event_fd = -1;
static thread_local bool                running    = false;
static thread_local bool                draining   = false;
static thread_local uint64_t            event_val  = 0;
static thread_local struct io_event_ctx sig_ctx    = {.type = EVENT_TYPE_SIGNAL};

bool server_is_draining(void) { return draining; }

#include "orbit/log.h"
#include "orbit/rtp_engine.h"
#include "orbit/sip_router.h"
#include "orbit/ws_bridge.h"

int server_submit_recv(struct io_event_ctx *restrict const ctx) {
    auto const sqe = io_uring_get_sqe(&ring);
    if (sqe == nullptr)
        return -1;
    io_uring_prep_recvmsg(sqe, ctx->fd, &ctx->msg, 0);
    io_uring_sqe_set_data(sqe, ctx);
    return io_uring_submit(&ring);
}

int server_submit_sendmsg(struct io_event_ctx *restrict const ctx) {
    auto const sqe = io_uring_get_sqe(&ring);
    if (sqe == nullptr)
        return -1;
    io_uring_prep_sendmsg(sqe, ctx->fd, &ctx->msg, 0);
    io_uring_sqe_set_data(sqe, ctx);
    return io_uring_submit(&ring);
}

int server_submit_accept(struct io_event_ctx *restrict const ctx) {
    auto const sqe = io_uring_get_sqe(&ring);
    if (sqe == nullptr)
        return -1;
    io_uring_prep_accept(
        sqe,
        ctx->fd,
        (struct sockaddr *)&ctx->remote_addr,
        &ctx->msg.msg_namelen,
        0);
    io_uring_sqe_set_data(sqe, ctx);
    return io_uring_submit(&ring);
}

int server_submit_send(struct io_event_ctx *restrict const ctx) {
    auto const sqe = io_uring_get_sqe(&ring);
    if (sqe == nullptr)
        return -1;
    io_uring_prep_send(sqe, ctx->fd, ctx->buffer, ctx->length, 0);
    io_uring_sqe_set_data(sqe, ctx);
    return io_uring_submit(&ring);
}

int server_init(void) {
    if (io_uring_queue_init(256, &ring, 0) < 0) {
        return -1;
    }

    return 0;
}

void server_stop(void) { running = false; }

void server_poll_once(void) {
    struct io_uring_cqe *cqe = nullptr;
    if (io_uring_wait_cqe(&ring, &cqe) < 0)
        return;

    if (io_uring_cqe_get_data(cqe) == &sig_ctx) {
        draining = true;
        LOGINF(
            "Server Caught Signal: Entering draining state. Waiting for %zu active calls to end.",
            sip_router_active_call_count());
    } else {
        auto const ctx = (struct io_event_ctx *)io_uring_cqe_get_data(cqe);
        if (ctx != nullptr) {
            if (ctx->type == EVENT_TYPE_RTP_RECV) {
                if (cqe->res > 0) {
                    rtp_engine_process_packet(ctx, (size_t)cqe->res);
                } else {
                    rtp_engine_free_ctx(ctx);
                }
            } else if (ctx->type == EVENT_TYPE_RTP_SEND) {
                rtp_engine_process_send_complete(ctx);
            } else if (ctx->type == EVENT_TYPE_WS_ACCEPT) {
                ws_bridge_process_accept(ctx, cqe->res);
            } else if (ctx->type == EVENT_TYPE_WS_RECV) {
                ws_bridge_process_recv(ctx, cqe->res);
            } else if (ctx->type == EVENT_TYPE_WS_SEND) {
                ws_bridge_process_send(ctx, cqe->res);
            } else if (ctx->type == EVENT_TYPE_SIP_RECV) {
                if (cqe->res > 0) {
                    sip_router_process_recv(ctx, (size_t)cqe->res);
                }
            } else if (ctx->type == EVENT_TYPE_SIP_SEND) {
                sip_router_process_send(ctx);
            }
        }
    }

    io_uring_cqe_seen(&ring, cqe);
}

void server_run(int event_fd) {
    running    = true;
    m_event_fd = event_fd;

    auto const sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, m_event_fd, &event_val, sizeof(event_val), 0);
    io_uring_sqe_set_data(sqe, &sig_ctx);
    io_uring_submit(&ring);

    while (running) {
        if (draining && sip_router_active_call_count() == 0) {
            LOGINF("Server Draining Complete: No active calls remaining. Shutting down.");
            break;
        }
        server_poll_once();
    }
}

void server_cleanup(void) { io_uring_queue_exit(&ring); }
