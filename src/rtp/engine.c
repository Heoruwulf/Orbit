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
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>
#include "orbit/config.h"
#include "orbit/log.h"
#include "orbit/macros.h"
#include "orbit/memory.h"
#include "orbit/rtp_engine.h"
#include "orbit/server.h"
#include "orbit/sip_router.h"
#include "orbit/thread.h"
#include "orbit/ws_bridge.h"

static size_t                   g_next_port_offset = 0;
static thread_local mem_pool_t *rtp_buffer_pool    = nullptr;
static thread_local mem_pool_t *rtp_ctx_pool       = nullptr;

constexpr size_t RTP_BUFFER_SIZE = 1500;

int rtp_engine_init(void) {
    if (g_config.rtp_min_port > g_config.rtp_max_port)
        return -1;

    rtp_buffer_pool = pool_create(
        (struct pool_config){.object_size = RTP_BUFFER_SIZE, .count = g_config.max_calls * 2});
    rtp_ctx_pool = pool_create((struct pool_config){.object_size = sizeof(struct io_event_ctx),
                                                    .count       = g_config.max_calls * 2});
    if (rtp_buffer_pool == nullptr || rtp_ctx_pool == nullptr) {
        LOGERR("RTP Engine: Failed to create memory pools");
        return -1;
    }

    return 0;
}

void rtp_engine_cleanup(void) {
    pool_destroy(rtp_buffer_pool);
    pool_destroy(rtp_ctx_pool);
    rtp_buffer_pool = nullptr;
    rtp_ctx_pool    = nullptr;
}

int rtp_engine_allocate_port(
    struct call_session *restrict const session,
    uint16_t *restrict const out_port) {

    size_t const total_ports = (size_t)(g_config.rtp_max_port - g_config.rtp_min_port + 1);
    int          fd          = -1;
    uint16_t     port        = 0;

    for (size_t i = 0; i < total_ports; ++i) {
        size_t const offset = __atomic_fetch_add(&g_next_port_offset, 1, __ATOMIC_RELAXED);
        port                = (uint16_t)(g_config.rtp_min_port + (offset % total_ports));

        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
            continue;

        int const flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int const opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {0};
        addr.sin_family         = AF_INET;
        inet_pton(
            AF_INET,
            g_config.rtp_listen_addr ? g_config.rtp_listen_addr : "0.0.0.0",
            &addr.sin_addr);
        addr.sin_port = htons(port);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            break; // Successfully bound
        }
        close(fd);
        fd = -1;
    }

    if (fd < 0) {
        LOGERR("RTP Engine: No free RTP ports available");
        return -1;
    }

    auto const buffer = pool_alloc(rtp_buffer_pool);
    auto const ctx    = (struct io_event_ctx *)pool_alloc(rtp_ctx_pool);

    if (buffer != nullptr && ctx != nullptr) {
        *ctx         = (struct io_event_ctx){};
        ctx->type    = EVENT_TYPE_RTP_RECV;
        ctx->fd      = fd;
        ctx->buffer  = buffer;
        ctx->length  = RTP_BUFFER_SIZE;
        ctx->session = session;

        ctx->iov.iov_base = buffer;
        ctx->iov.iov_len  = RTP_BUFFER_SIZE;

        ctx->msg.msg_name    = &ctx->remote_addr;
        ctx->msg.msg_namelen = sizeof(ctx->remote_addr);
        ctx->msg.msg_iov     = &ctx->iov;
        ctx->msg.msg_iovlen  = 1;

        server_submit_recv(ctx);
    } else {
        LOGERR("RTP Engine: Failed to allocate buffer or context for RTP port %u", port);
        if (buffer)
            pool_free(rtp_buffer_pool, buffer);
        if (ctx)
            pool_free(rtp_ctx_pool, ctx);
        close(fd);
        return -1;
    }

    LOGINF("RTP Engine: Allocated port %u", port);
    *out_port = port;
    return fd;
}

void rtp_engine_free_port(int const fd, uint16_t const port) {
    if (fd >= 0) {
        LOGINF("RTP Engine: Freeing port %u", port);
        close(fd);
    }
}

void rtp_engine_free_ctx(struct io_event_ctx *restrict const ctx) {
    if (ctx == nullptr)
        return;
    pool_free(rtp_buffer_pool, ctx->buffer);
    pool_free(rtp_ctx_pool, ctx);
}

void rtp_engine_process_send_complete(struct io_event_ctx *restrict const ctx) {
    rtp_engine_free_ctx(ctx);
}

bool rtp_generate_silence(
    struct call_session *restrict const session,
    void *restrict const buffer,
    size_t *restrict const out_length) {
    if (session == nullptr || buffer == nullptr || out_length == nullptr)
        return false;

    uint8_t  pt           = 0;
    uint8_t  fill_byte    = 0xFF; // PCMU silence
    size_t   payload_len  = 160;
    uint32_t ts_increment = 160;

    if (session->remote_sdp.pcmu.payload_type != 255) {
        pt        = session->remote_sdp.pcmu.payload_type;
        fill_byte = 0xFF;
    } else if (session->remote_sdp.pcma.payload_type != 255) {
        pt        = session->remote_sdp.pcma.payload_type;
        fill_byte = 0xD5;
    } else if (session->remote_sdp.l16.payload_type != 255) {
        pt           = session->remote_sdp.l16.payload_type;
        fill_byte    = 0x00;
        payload_len  = 320;
        ts_increment = 320;
    } else {
        return false;
    }

    uint8_t *bytes = (uint8_t *)buffer;
    bytes[0]       = 0x80; // V=2
    bytes[1]       = pt;

    bytes[2] = (uint8_t)(session->tx_seq_num >> 8);
    bytes[3] = (uint8_t)(session->tx_seq_num & 0xFF);

    bytes[4] = (uint8_t)(session->tx_timestamp >> 24);
    bytes[5] = (uint8_t)(session->tx_timestamp >> 16);
    bytes[6] = (uint8_t)(session->tx_timestamp >> 8);
    bytes[7] = (uint8_t)(session->tx_timestamp & 0xFF);

    bytes[8]  = (uint8_t)(session->tx_ssrc >> 24);
    bytes[9]  = (uint8_t)(session->tx_ssrc >> 16);
    bytes[10] = (uint8_t)(session->tx_ssrc >> 8);
    bytes[11] = (uint8_t)(session->tx_ssrc & 0xFF);

    memset(bytes + 12, fill_byte, payload_len);
    *out_length = 12 + payload_len;

    session->tx_seq_num++;
    session->tx_timestamp += ts_increment;

    return true;
}

bool rtp_engine_send_silence(struct call_session *restrict const session, int const fd) {
    if (session == nullptr || !session->has_learned_remote_addr)
        return false;

    auto const buffer = pool_alloc(rtp_buffer_pool);
    auto const ctx    = (struct io_event_ctx *)pool_alloc(rtp_ctx_pool);
    if (buffer == nullptr || ctx == nullptr) {
        if (buffer)
            pool_free(rtp_buffer_pool, buffer);
        if (ctx)
            pool_free(rtp_ctx_pool, ctx);
        return false;
    }

    size_t length = 0;
    if (!rtp_generate_silence(session, buffer, &length)) {
        pool_free(rtp_buffer_pool, buffer);
        pool_free(rtp_ctx_pool, ctx);
        return false;
    }

    *ctx             = (struct io_event_ctx){};
    ctx->type        = EVENT_TYPE_RTP_SEND;
    ctx->fd          = fd;
    ctx->buffer      = buffer;
    ctx->length      = length;
    ctx->session     = session;
    ctx->remote_addr = session->learned_remote_addr;

    ctx->iov.iov_base = buffer;
    ctx->iov.iov_len  = length;

    ctx->msg.msg_name    = &ctx->remote_addr;
    ctx->msg.msg_namelen = sizeof(ctx->remote_addr);
    ctx->msg.msg_iov     = &ctx->iov;
    ctx->msg.msg_iovlen  = 1;

    server_submit_sendmsg(ctx);
    return true;
}

bool rtp_engine_send_dtmf(
    struct call_session *restrict const session,
    int const      fd,
    uint8_t const  digit,
    bool const     is_start,
    bool const     is_end,
    uint16_t const duration) {
    if (session == nullptr || !session->has_learned_remote_addr)
        return false;

    auto const buffer = pool_alloc(rtp_buffer_pool);
    auto const ctx    = (struct io_event_ctx *)pool_alloc(rtp_ctx_pool);
    if (buffer == nullptr || ctx == nullptr) {
        if (buffer)
            pool_free(rtp_buffer_pool, buffer);
        if (ctx)
            pool_free(rtp_ctx_pool, ctx);
        return false;
    }

    size_t length = 0;
    if (!rtp_generate_dtmf(session, digit, is_start, is_end, duration, buffer, &length)) {
        pool_free(rtp_buffer_pool, buffer);
        pool_free(rtp_ctx_pool, ctx);
        return false;
    }

    *ctx             = (struct io_event_ctx){};
    ctx->type        = EVENT_TYPE_RTP_SEND;
    ctx->fd          = fd;
    ctx->buffer      = buffer;
    ctx->length      = length;
    ctx->session     = session;
    ctx->remote_addr = session->learned_remote_addr;

    ctx->iov.iov_base = buffer;
    ctx->iov.iov_len  = length;

    ctx->msg.msg_name    = &ctx->remote_addr;
    ctx->msg.msg_namelen = sizeof(ctx->remote_addr);
    ctx->msg.msg_iov     = &ctx->iov;
    ctx->msg.msg_iovlen  = 1;

    server_submit_sendmsg(ctx);
    return true;
}

bool rtp_engine_send_payload(
    struct call_session *restrict const session,
    int const fd,
    uint8_t const *restrict const payload,
    size_t const payload_len) {
    if (session == nullptr || !session->has_learned_remote_addr || payload == nullptr)
        return false;

    auto const buffer = pool_alloc(rtp_buffer_pool);
    auto const ctx    = (struct io_event_ctx *)pool_alloc(rtp_ctx_pool);
    if (buffer == nullptr || ctx == nullptr) {
        if (buffer)
            pool_free(rtp_buffer_pool, buffer);
        if (ctx)
            pool_free(rtp_ctx_pool, ctx);
        return false;
    }

    uint8_t pt = 0;
    if (session->remote_sdp.opus.payload_type != 255) {
        pt = session->remote_sdp.opus.payload_type;
    } else if (session->remote_sdp.pcmu.payload_type != 255) {
        pt = session->remote_sdp.pcmu.payload_type;
    } else if (session->remote_sdp.pcma.payload_type != 255) {
        pt = session->remote_sdp.pcma.payload_type;
    } else if (session->remote_sdp.l16.payload_type != 255) {
        pt = session->remote_sdp.l16.payload_type;
    } else {
        pool_free(rtp_buffer_pool, buffer);
        pool_free(rtp_ctx_pool, ctx);
        return false;
    }

    uint8_t *bytes = (uint8_t *)buffer;
    bytes[0]       = 0x80; // V=2
    bytes[1]       = pt;

    bytes[2] = (uint8_t)(session->tx_seq_num >> 8);
    bytes[3] = (uint8_t)(session->tx_seq_num & 0xFF);

    bytes[4] = (uint8_t)(session->tx_timestamp >> 24);
    bytes[5] = (uint8_t)(session->tx_timestamp >> 16);
    bytes[6] = (uint8_t)(session->tx_timestamp >> 8);
    bytes[7] = (uint8_t)(session->tx_timestamp & 0xFF);

    bytes[8]  = (uint8_t)(session->tx_ssrc >> 24);
    bytes[9]  = (uint8_t)(session->tx_ssrc >> 16);
    bytes[10] = (uint8_t)(session->tx_ssrc >> 8);
    bytes[11] = (uint8_t)(session->tx_ssrc & 0xFF);

    size_t copy_len = payload_len;
    if (copy_len > RTP_BUFFER_SIZE - 12) {
        copy_len = RTP_BUFFER_SIZE - 12;
    }
    __builtin_memcpy(bytes + 12, payload, copy_len);

    *ctx             = (struct io_event_ctx){};
    ctx->type        = EVENT_TYPE_RTP_SEND;
    ctx->fd          = fd;
    ctx->buffer      = buffer;
    ctx->length      = 12 + copy_len;
    ctx->session     = session;
    ctx->remote_addr = session->learned_remote_addr;

    ctx->iov.iov_base = buffer;
    ctx->iov.iov_len  = 12 + copy_len;

    ctx->msg.msg_name    = &ctx->remote_addr;
    ctx->msg.msg_namelen = sizeof(ctx->remote_addr);
    ctx->msg.msg_iov     = &ctx->iov;
    ctx->msg.msg_iovlen  = 1;

    server_submit_sendmsg(ctx);

    session->tx_seq_num++;
    session->tx_timestamp += (uint32_t)copy_len; // Approx timestamp increment

    return true;
}

void rtp_engine_process_packet(struct io_event_ctx *restrict const ctx, size_t const bytes_read) {
    if (likely(bytes_read > 12)) {
        // Minimum RTP header size is 12 bytes. Skip the header and send the payload.
        ws_bridge_send_binary(ctx->session, (uint8_t const *)ctx->buffer + 12, bytes_read - 12);
    }

    if (unlikely(ctx->session != nullptr && !ctx->session->has_learned_remote_addr)) {
        ctx->session->has_learned_remote_addr = true;
        ctx->session->learned_remote_addr     = ctx->remote_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(ctx->remote_addr.sin_addr), ip, INET_ADDRSTRLEN);
        LOGINF(
            "RTP Engine: Learned remote peer for Call-ID %.*s -> %s:%d",
            (int)ctx->session->call_id_len,
            ctx->session->call_id_buf,
            ip,
            ntohs(ctx->remote_addr.sin_port));
    }

    // Re-queue immediately to catch the next packet
    server_submit_recv(ctx);
}
