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
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>
#include "orbit/config.h"
#include "orbit/log.h"
#include "orbit/memory.h"
#include "orbit/rtp_engine.h"
#include "orbit/server.h"
#include "orbit/sip_router.h"
#include "orbit/ws_bridge.h"
#include "orbit/ws_handler.h"

struct ws_bridge_session {
    struct ws_connection conn;
    struct call_session *sip_call;
    bool                 handshaked;
    struct io_event_ctx  recv_ctx;
    struct io_event_ctx  send_ctx;
    uint8_t              buffer[4096];
    uint8_t              send_buffer[4096];
    struct string_view   unread_sv;
    bool                 send_pending;
    bool                 recv_pending;
};

static thread_local mem_pool_t         *ws_pool      = nullptr;
static thread_local int                 ws_server_fd = -1;
static thread_local struct io_event_ctx accept_ctx   = {.type = EVENT_TYPE_WS_ACCEPT};

// WSLAY CALLBACKS
static ssize_t
recv_callback(wslay_event_context_ptr ctx, uint8_t *buf, size_t len, int flags, void *user_data) {
    (void)ctx;
    (void)flags;
    struct ws_bridge_session *session = (struct ws_bridge_session *)user_data;
    if (session->unread_sv.length == 0) {
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }
    size_t const copy_len = len < session->unread_sv.length ? len : session->unread_sv.length;
    __builtin_memcpy(buf, session->unread_sv.data, copy_len);
    session->unread_sv.data += copy_len;
    session->unread_sv.length -= copy_len;
    return (ssize_t)copy_len;
}

static ssize_t send_callback(
    wslay_event_context_ptr ctx,
    const uint8_t          *data,
    size_t                  len,
    int                     flags,
    void                   *user_data) {
    (void)flags;
    struct ws_bridge_session *session = (struct ws_bridge_session *)user_data;

    // If an io_uring SQE is already in-flight, we MUST block wslay from sending more.
    if (session->send_pending) {
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }

    // Copy the payload into our pre-allocated memory slab so wslay can free its internal pointer.
    size_t const copy_len = len < sizeof(session->send_buffer) ? len : sizeof(session->send_buffer);
    __builtin_memcpy(session->send_buffer, data, copy_len);

    session->send_ctx.buffer = session->send_buffer;
    session->send_ctx.length = copy_len;
    session->send_pending    = true;

    if (server_submit_send(&session->send_ctx) < 0) {
        session->send_pending = false;
        wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
        return -1;
    }

    return (ssize_t)copy_len;
}

static void on_msg_recv_callback(
    wslay_event_context_ptr                   ctx,
    const struct wslay_event_on_msg_recv_arg *arg,
    void                                     *user_data) {
    (void)ctx;
    struct ws_bridge_session *session = (struct ws_bridge_session *)user_data;
    if (arg->opcode == WSLAY_TEXT_FRAME) {
        struct string_view msg   = SV_INIT_LEN((char *)arg->msg, arg->msg_length);
        uint8_t            digit = 0;
        if (ws_parse_dtmf_json(msg, &digit)) {
            rtp_engine_send_dtmf(
                session->sip_call,
                session->sip_call->rtp_fd,
                digit,
                true,
                false,
                160);
        }
    } else if (arg->opcode == WSLAY_BINARY_FRAME) {
        if (session->sip_call != nullptr && session->sip_call->is_active) {
            rtp_engine_send_payload(
                session->sip_call,
                session->sip_call->rtp_fd,
                arg->msg,
                arg->msg_length);
        }
    }
}

static struct wslay_event_callbacks const wslay_cbs =
    {recv_callback, send_callback, nullptr, nullptr, nullptr, nullptr, on_msg_recv_callback};

int ws_bridge_init(void) {
    ws_pool = pool_create((struct pool_config){.object_size = sizeof(struct ws_bridge_session),
                                               .count       = g_config.max_calls});
    if (ws_pool == nullptr) {
        LOGERR("WS Bridge: Failed to create ws_pool");
        return -1;
    }

    // WARMUP PHASE: wslay_event_context_server_init dynamically allocates memory using
    // malloc/calloc. To maintain zero-allocation (zero page faults) in the hot path, we initialize
    // and free max_calls worth of contexts upfront. This forces glibc to request and prefault the
    // pages from the kernel, placing them into the free list for immediate cache-hot reuse during
    // load.
    LOGINF("WS Bridge: Warming up WebSocket memory arenas...");
    wslay_event_context_ptr *warmup_ctxs =
        (wslay_event_context_ptr *)calloc(g_config.max_calls, sizeof(wslay_event_context_ptr));
    if (warmup_ctxs != nullptr) {
        for (size_t i = 0; i < g_config.max_calls; ++i) {
            wslay_event_context_server_init(&warmup_ctxs[i], &wslay_cbs, nullptr);
        }
        for (size_t i = 0; i < g_config.max_calls; ++i) {
            if (warmup_ctxs[i] != nullptr) {
                wslay_event_context_free(warmup_ctxs[i]);
            }
        }
        free((void *)warmup_ctxs);
    }

    ws_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ws_server_fd < 0) {
        LOGERR("WS Bridge: Failed to create WebSocket server socket");
        return -1;
    }
    int opt = 1;
    setsockopt(ws_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(ws_server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    int const flags = fcntl(ws_server_fd, F_GETFL, 0);
    fcntl(ws_server_fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {0};
    addr.sin_family         = AF_INET;
    inet_pton(
        AF_INET,
        g_config.ws_listen_addr ? g_config.ws_listen_addr : "0.0.0.0",
        &addr.sin_addr);
    addr.sin_port = htons(g_config.ws_listen_port);

    if (bind(ws_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGERR("WS Bridge: Failed to bind WebSocket server socket");
        return -1;
    }
    if (listen(ws_server_fd, 128) < 0) {
        LOGERR("WS Bridge: Failed to listen on WebSocket server socket");
        return -1;
    }

    accept_ctx.fd = ws_server_fd;
    server_submit_accept(&accept_ctx);
    return 0;
}

void ws_bridge_cleanup(void) {
    if (ws_server_fd >= 0)
        close(ws_server_fd);
    pool_destroy(ws_pool);
}

void ws_bridge_process_accept(struct io_event_ctx *restrict const ctx, int const res) {
    server_submit_accept(ctx); // Re-queue accept
    if (res < 0) {
        LOGERR("WS Bridge: Accept failed with error code %d", res);
        return;
    }

    int const flags = fcntl(res, F_GETFL, 0);
    fcntl(res, F_SETFL, flags | O_NONBLOCK);

    LOGINF("WS Bridge: Accepted new WebSocket client connection (FD: %d)", res);

    auto const session = (struct ws_bridge_session *)pool_alloc(ws_pool);
    if (session == nullptr) {
        LOGERR("WS Bridge: Failed to allocate session for new connection (FD: %d)", res);
        close(res);
        return;
    }

    *session         = (struct ws_bridge_session){};
    session->conn.fd = res;

    session->recv_ctx.type    = EVENT_TYPE_WS_RECV;
    session->recv_ctx.fd      = res;
    session->recv_ctx.buffer  = session->buffer;
    session->recv_ctx.length  = sizeof(session->buffer);
    session->recv_ctx.session = (struct call_session *)session; // Trick

    session->recv_ctx.iov.iov_base   = session->buffer;
    session->recv_ctx.iov.iov_len    = sizeof(session->buffer);
    session->recv_ctx.msg.msg_iov    = &session->recv_ctx.iov;
    session->recv_ctx.msg.msg_iovlen = 1;

    session->send_ctx.type    = EVENT_TYPE_WS_SEND;
    session->send_ctx.fd      = res;
    session->send_ctx.session = (struct call_session *)session;

    session->recv_pending = true;
    server_submit_recv(&session->recv_ctx);
}

void ws_bridge_process_recv(struct io_event_ctx *restrict const ctx, int const res) {
    auto const session    = (struct ws_bridge_session *)ctx->session;
    session->recv_pending = false;

    if (res <= 0) {
        if (session->conn.fd >= 0) {
            LOGINF("WS Bridge: Connection closed (FD: %d)", session->conn.fd);
            if (session->handshaked) {
                if (session->sip_call)
                    call_lock(session->sip_call);
                ws_connection_free(&session->conn);
                if (session->sip_call != nullptr) {
                    auto const sip                = session->sip_call;
                    session->sip_call->ws_session = nullptr;
                    session->sip_call             = nullptr;
                    call_unlock(sip);
                    call_release(sip);
                }
            }
            close(session->conn.fd);
            session->conn.fd = -1;
        }

        if (!session->send_pending) {
            pool_free(ws_pool, session);
        }
        return;
    }

    if (!session->handshaked) {
        struct string_view const raw         = SV_INIT_LEN((char *)session->buffer, (size_t)res);
        struct string_view       internal_id = {};
        if (ws_parse_handshake_url(raw, &internal_id)) {
            auto const sip = sip_router_find_call_by_internal_id(internal_id);
            if (sip != nullptr) {
                LOGINF(
                    "WS Bridge: Handshake successful, mapped to Call-ID: %.*s",
                    (int)sip->call_id_len,
                    sip->call_id_buf);
                session->sip_call   = sip;
                session->handshaked = true;
                sip->ws_session     = session;
                ws_connection_init(&session->conn, session->conn.fd, &wslay_cbs, session);

                char const *resp =
                    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                    "Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
                send(session->conn.fd, resp, __builtin_strlen(resp), MSG_NOSIGNAL);

                // Send JSON Metadata
                char         json_buf[512];
                size_t const json_len = ws_generate_metadata_json(sip, json_buf, sizeof(json_buf));
                struct wslay_event_msg msg = {WSLAY_TEXT_FRAME, (uint8_t *)json_buf, json_len};
                call_lock(sip);
                wslay_event_queue_msg(session->conn.ctx, &msg);
                wslay_event_send(session->conn.ctx);
                call_unlock(sip);
            } else {
                LOGERR(
                    "WS Bridge: Handshake failed, unknown Internal ID: %.*s",
                    (int)internal_id.length,
                    internal_id.data);
                if (session->conn.fd >= 0) {
                    close(session->conn.fd);
                    session->conn.fd = -1;
                }
                if (!session->send_pending) {
                    pool_free(ws_pool, session);
                }
                return;
            }
        }
    } else {
        session->unread_sv = SV_LEN((char *)session->buffer, (size_t)res);
        if (session->sip_call)
            call_lock(session->sip_call);
        wslay_event_recv(session->conn.ctx);
        wslay_event_send(session->conn.ctx); // In case it needs to send PONG or responses
        if (session->sip_call)
            call_unlock(session->sip_call);
    }

    session->recv_pending = true;
    server_submit_recv(&session->recv_ctx);
}

void ws_bridge_process_send(struct io_event_ctx *restrict const ctx, int const res) {
    auto const session    = (struct ws_bridge_session *)ctx->session;
    session->send_pending = false;

    if (res <= 0) {
        if (session->conn.fd >= 0) {
            if (session->handshaked) {
                if (session->sip_call)
                    call_lock(session->sip_call);
                ws_connection_free(&session->conn);
                if (session->sip_call != nullptr) {
                    auto const sip                = session->sip_call;
                    session->sip_call->ws_session = nullptr;
                    session->sip_call             = nullptr;
                    call_unlock(sip);
                    call_release(sip);
                }
            }
            close(session->conn.fd);
            session->conn.fd = -1;
        }

        if (!session->recv_pending) {
            pool_free(ws_pool, session);
        }
        return;
    }

    // The previous chunk finished sending over io_uring. Resume wslay.
    if (session->handshaked) {
        if (session->sip_call)
            call_lock(session->sip_call);
        wslay_event_send(session->conn.ctx);
        if (session->sip_call)
            call_unlock(session->sip_call);
    }
}

void ws_bridge_send_binary(struct call_session *sip_call, uint8_t const *data, size_t len) {
    if (sip_call == nullptr || sip_call->ws_session == nullptr || data == nullptr || len == 0)
        return;

    auto const session = (struct ws_bridge_session *)sip_call->ws_session;
    if (session->handshaked) {
        struct wslay_event_msg msg = {WSLAY_BINARY_FRAME, (uint8_t *)data, len};
        call_lock(sip_call);
        wslay_event_queue_msg(session->conn.ctx, &msg);
        wslay_event_send(session->conn.ctx);
        call_unlock(sip_call);
    }
}

void ws_bridge_close(struct call_session *sip_call) {
    if (sip_call == nullptr || sip_call->ws_session == nullptr)
        return;

    auto const session = (struct ws_bridge_session *)sip_call->ws_session;
    call_lock(sip_call);
    if (session->handshaked) {
        LOGINF("WS Bridge: Closing connection to client");
        shutdown(session->conn.fd, SHUT_RDWR);
    }
    // Do not set sip_call->ws_session = nullptr here.
    // The WS thread will wake up from the shutdown EOF and clean up safely on its own thread.
    call_unlock(sip_call);
}
