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
#include "orbit/event.h"
#include "orbit/server.h"
#include "orbit/sip_router.h"
#include "orbit/ws_bridge.h"

sip_verb_t sip_parse_verb(struct string_view const method) {
    if (__builtin_expect(method.length < 3 || method.data == nullptr, 0)) {
        return SIP_VERB_UNKNOWN;
    }

    auto const d = method.data;

    // Fast zero-copy prefix matching
    if (method.length >= 6 && memcmp(d, "INVITE", 6) == 0)
        return SIP_VERB_INVITE;
    if (method.length >= 3 && memcmp(d, "ACK", 3) == 0)
        return SIP_VERB_ACK;
    if (method.length >= 6 && memcmp(d, "CANCEL", 6) == 0)
        return SIP_VERB_CANCEL;
    if (method.length >= 3 && memcmp(d, "BYE", 3) == 0)
        return SIP_VERB_BYE;
    if (method.length >= 7 && memcmp(d, "OPTIONS", 7) == 0)
        return SIP_VERB_OPTIONS;
    if (method.length >= 7 && memcmp(d, "SIP/2.0", 7) == 0)
        return SIP_VERB_RESPONSE;

    return SIP_VERB_UNKNOWN;
}

static bool
parse_header_line(struct string_view const line, struct sip_message *restrict const out_msg) {
    if (line.length > 9 && memcmp(line.data, "Call-ID: ", 9) == 0) {
        out_msg->call_id.data   = line.data + 9;
        out_msg->call_id.length = line.length - 9;
    } else if (line.length > 6 && memcmp(line.data, "From: ", 6) == 0) {
        out_msg->from_tag.data   = line.data + 6;
        out_msg->from_tag.length = line.length - 6;
    } else if (line.length > 4 && memcmp(line.data, "To: ", 4) == 0) {
        out_msg->to_tag.data   = line.data + 4;
        out_msg->to_tag.length = line.length - 4;
    } else if (line.length > 6 && memcmp(line.data, "CSeq: ", 6) == 0) {
        out_msg->cseq.data   = line.data + 6;
        out_msg->cseq.length = line.length - 6;
    } else if (line.length > 5 && memcmp(line.data, "Via: ", 5) == 0) {
        out_msg->via.data   = line.data + 5;
        out_msg->via.length = line.length - 5;
    } else if (line.length > 2 && line.data[0] == 'X' && line.data[1] == '-') {
        if (out_msg->custom_header_count < SIP_MAX_CUSTOM_HEADERS) {
            char const *colon = memchr(line.data, ':', line.length);
            if (colon != nullptr) {
                size_t const name_len = (size_t)(colon - line.data);
                char const  *val      = colon + 1;
                size_t       val_len  = line.length - name_len - 1;
                while (val_len > 0 && (*val == ' ' || *val == '\t')) {
                    val++;
                    val_len--;
                }
                out_msg->custom_headers[out_msg->custom_header_count].name.data    = line.data;
                out_msg->custom_headers[out_msg->custom_header_count].name.length  = name_len;
                out_msg->custom_headers[out_msg->custom_header_count].value.data   = val;
                out_msg->custom_headers[out_msg->custom_header_count].value.length = val_len;
                out_msg->custom_header_count++;
            }
        }
    }
    return true;
}

bool sip_parse_message(struct string_view const raw, struct sip_message *restrict const out_msg) {
    if (__builtin_expect(raw.data == nullptr || raw.length == 0 || out_msg == nullptr, 0))
        return false;

    *out_msg = (struct sip_message){};

    char const *restrict ptr = raw.data;
    char const *const end    = raw.data + raw.length;

    // Parse start line
    char const *line_end = memchr(ptr, '\r', (size_t)(end - ptr));
    if (line_end == nullptr || line_end + 1 >= end || *(line_end + 1) != '\n') {
        return false;
    }

    struct string_view const first_line = {.data = ptr, .length = (size_t)(line_end - ptr)};
    out_msg->verb                       = sip_parse_verb(first_line);

    ptr = line_end + 2;

    // Parse headers
    while (ptr < end) {
        if (ptr + 1 < end && ptr[0] == '\r' && ptr[1] == '\n') {
            ptr += 2;
            out_msg->body.data   = ptr;
            out_msg->body.length = (size_t)(end - ptr);
            return true; // Reached body
        }

        line_end = memchr(ptr, '\r', (size_t)(end - ptr));
        if (line_end == nullptr || line_end + 1 >= end || *(line_end + 1) != '\n') {
            return false;
        }

        struct string_view const line = {.data = ptr, .length = (size_t)(line_end - ptr)};
        parse_header_line(line, out_msg);

        ptr = line_end + 2;
    }

    return true;
}

#include "orbit/log.h"
#include "orbit/memory.h"
#include "orbit/rtp_engine.h"

static thread_local mem_pool_t *call_pool    = nullptr;
static struct call_session    **active_calls = nullptr;

static thread_local int                 sip_fd       = -1;
static thread_local struct io_event_ctx sip_recv_ctx = {.type = EVENT_TYPE_SIP_RECV};
static thread_local uint8_t             sip_recv_buffer[4096];

static thread_local struct io_event_ctx sip_send_ctx = {.type = EVENT_TYPE_SIP_SEND};
static thread_local uint8_t             sip_send_buffer[4096];

int sip_router_init(void) {
    if (__atomic_load_n(&active_calls, __ATOMIC_ACQUIRE) == nullptr) {
        auto const new_arr =
            (struct call_session **)calloc(g_config.max_calls, sizeof(struct call_session *));
        struct call_session **expected = nullptr;
        if (!__atomic_compare_exchange_n(
                &active_calls,
                &expected,
                new_arr,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE))
        {
            free(new_arr);
        }
    }
    if (__atomic_load_n(&active_calls, __ATOMIC_ACQUIRE) == nullptr) {
        LOGERR("SIP Router: Failed to allocate active_calls array");
        return -1;
    }

    call_pool = pool_create((struct pool_config){.object_size = sizeof(struct call_session),
                                                 .count       = g_config.max_calls});
    if (call_pool == nullptr) {
        LOGERR("SIP Router: Failed to create call pool");
        return -1;
    }

    sip_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sip_fd < 0) {
        LOGERR("SIP Router: Failed to create SIP socket");
        return -1;
    }

    int opt = 1;
    setsockopt(sip_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sip_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family         = AF_INET;
    addr.sin_port           = htons(g_config.sip_listen_port);
    inet_pton(
        AF_INET,
        g_config.sip_listen_addr ? g_config.sip_listen_addr : "0.0.0.0",
        &addr.sin_addr);

    if (bind(sip_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGERR("SIP Router: Failed to bind SIP socket on port %d", g_config.sip_listen_port);
        close(sip_fd);
        return -1;
    }

    sip_recv_ctx.fd              = sip_fd;
    sip_recv_ctx.buffer          = sip_recv_buffer;
    sip_recv_ctx.length          = sizeof(sip_recv_buffer);
    sip_recv_ctx.msg.msg_name    = &sip_recv_ctx.remote_addr;
    sip_recv_ctx.msg.msg_namelen = sizeof(sip_recv_ctx.remote_addr);
    sip_recv_ctx.iov.iov_base    = sip_recv_buffer;
    sip_recv_ctx.iov.iov_len     = sizeof(sip_recv_buffer);
    sip_recv_ctx.msg.msg_iov     = &sip_recv_ctx.iov;
    sip_recv_ctx.msg.msg_iovlen  = 1;

    sip_send_ctx.fd             = sip_fd;
    sip_send_ctx.buffer         = sip_send_buffer;
    sip_send_ctx.msg.msg_name   = &sip_send_ctx.remote_addr;
    sip_send_ctx.iov.iov_base   = sip_send_buffer;
    sip_send_ctx.msg.msg_iov    = &sip_send_ctx.iov;
    sip_send_ctx.msg.msg_iovlen = 1;

    server_submit_recv(&sip_recv_ctx);

    return 0;
}

struct call_session *sip_router_find_call(struct string_view const call_id) {
    struct call_session **arr = __atomic_load_n(&active_calls, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < g_config.max_calls; ++i) {
        auto const session = __atomic_load_n(&arr[i], __ATOMIC_ACQUIRE);
        if (session != nullptr && session->is_active) {
            if (session->call_id_len == call_id.length &&
                memcmp(session->call_id_buf, call_id.data, call_id.length) == 0)
            {
                __atomic_fetch_add(&session->refcount, 1, __ATOMIC_RELAXED);
                return session;
            }
        }
    }
    return nullptr;
}

struct call_session *sip_router_find_call_by_internal_id(struct string_view const internal_id) {
    struct call_session **arr = __atomic_load_n(&active_calls, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < g_config.max_calls; ++i) {
        auto const session = __atomic_load_n(&arr[i], __ATOMIC_ACQUIRE);
        if (session != nullptr && session->is_active) {
            if (session->internal_id_len == internal_id.length &&
                memcmp(session->internal_id_buf, internal_id.data, internal_id.length) == 0)
            {
                __atomic_fetch_add(&session->refcount, 1, __ATOMIC_RELAXED);
                return session;
            }
        }
    }
    return nullptr;
}

size_t sip_router_active_call_count(void) {
    struct call_session **arr = __atomic_load_n(&active_calls, __ATOMIC_ACQUIRE);
    if (arr == nullptr)
        return 0;

    size_t count = 0;
    for (size_t i = 0; i < g_config.max_calls; ++i) {
        struct call_session *s = __atomic_load_n(&arr[i], __ATOMIC_ACQUIRE);
        if (s != nullptr && s->is_active) {
            count++;
        }
    }
    return count;
}

static uint64_t prng_state = 0x1234567890abcdef;
static uint64_t xorshift64(void) {
    uint64_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return prng_state = x;
}

static void generate_uuid(char *out) {
    uint64_t r1 = xorshift64();
    uint64_t r2 = xorshift64();
    uint8_t *b1 = (uint8_t *)&r1;
    uint8_t *b2 = (uint8_t *)&r2;
    snprintf(
        out,
        37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b1[0],
        b1[1],
        b1[2],
        b1[3],
        b1[4],
        b1[5],
        (b1[6] & 0x0f) | 0x40,
        b1[7],
        (b2[0] & 0x3f) | 0x80,
        b2[1],
        b2[2],
        b2[3],
        b2[4],
        b2[5],
        b2[6],
        b2[7]);
}

static struct call_session *create_call(struct string_view const call_id) {
    auto const session = (struct call_session *)pool_alloc(call_pool);
    if (session == nullptr)
        return nullptr;

    *session              = (struct call_session){};
    size_t const copy_len = call_id.length < sizeof(session->call_id_buf)
                                ? call_id.length
                                : sizeof(session->call_id_buf);
    __builtin_memcpy(session->call_id_buf, call_id.data, copy_len);
    session->call_id_len = copy_len;
    generate_uuid(session->internal_id_buf);
    session->internal_id_len = 36;
    session->is_active       = true;
    session->refcount        = 2;

    // Insert into active calls
    struct call_session **arr = __atomic_load_n(&active_calls, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < g_config.max_calls; ++i) {
        struct call_session *expected = nullptr;
        if (__atomic_compare_exchange_n(
                &arr[i],
                &expected,
                session,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_RELAXED))
        {
            return session;
        }
    }

    pool_free(call_pool, session);
    return nullptr; // Array full
}

static void destroy_call(struct call_session *const session) {
    if (session == nullptr)
        return;
    session->is_active = false;

    if (session->rtp_fd >= 0) {
        rtp_engine_free_port(session->rtp_fd, session->local_sdp.port);
        session->rtp_fd = -1;
    }

    if (session->ws_session != nullptr) {
        ws_bridge_close(session);
    }

    // Remove from active calls
    struct call_session **arr = __atomic_load_n(&active_calls, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < g_config.max_calls; ++i) {
        if (__atomic_load_n(&arr[i], __ATOMIC_ACQUIRE) == session) {
            __atomic_store_n(&arr[i], nullptr, __ATOMIC_RELEASE);
            break;
        }
    }
    call_release(session);
}

void call_release(struct call_session *call) {
    if (call == nullptr)
        return;
    if (__atomic_fetch_sub(&call->refcount, 1, __ATOMIC_ACQ_REL) == 1) {
        pool_free(call_pool, call);
    }
}

struct call_session *sip_router_process(struct sip_message const *restrict const msg) {
    if (msg == nullptr)
        return nullptr;

    if (msg->verb == SIP_VERB_OPTIONS) {
        // Just return a 200 OK ping immediately, don't spawn a session.
        return nullptr;
    }

    auto session = sip_router_find_call(msg->call_id);

    if (msg->verb == SIP_VERB_INVITE) {
        if (session == nullptr) {
            session = create_call(msg->call_id);
            if (session == nullptr) {
                LOGERR("SIP Router: Failed to create call session (Pool Exhausted)");
                return nullptr;
            }
            LOGINF(
                "SIP Router: Created new call session (Call-ID: %.*s)",
                (int)msg->call_id.length,
                msg->call_id.data);
            event_publish_call_answered(
                msg,
                (struct string_view){session->internal_id_buf, session->internal_id_len});
        }
        if (msg->body.length > 0) {
            sdp_parse(msg->body, &session->remote_sdp);
            uint8_t pt = 255;
            if (session->remote_sdp.opus.payload_type != 255)
                pt = session->remote_sdp.opus.payload_type;
            else if (session->remote_sdp.pcmu.payload_type != 255)
                pt = session->remote_sdp.pcmu.payload_type;
            else if (session->remote_sdp.pcma.payload_type != 255)
                pt = session->remote_sdp.pcma.payload_type;
            else if (session->remote_sdp.l16.payload_type != 255)
                pt = session->remote_sdp.l16.payload_type;

            char const *codec_name = rtp_get_codec_name(pt, &session->remote_sdp);

            LOGINF(
                "SIP Router: Call-ID %.*s negotiated codec %s (pt=%u), ptime=%u",
                (int)msg->call_id.length,
                msg->call_id.data,
                codec_name,
                pt,
                session->remote_sdp.ptime);
        }
        return session;
    }

    if (msg->verb == SIP_VERB_ACK) {
        LOGINF(
            "SIP Router: Received ACK (Call-ID: %.*s)",
            (int)msg->call_id.length,
            msg->call_id.data);
        // Acknowledgement for an established call, route it through
        return session;
    } else if (msg->verb == SIP_VERB_BYE || msg->verb == SIP_VERB_CANCEL) {
        LOGINF(
            "SIP Router: Received BYE/CANCEL (Call-ID: %.*s)",
            (int)msg->call_id.length,
            msg->call_id.data);
        if (session != nullptr) {
            LOGINF(
                "SIP Router: Destroying call session (Call-ID: %.*s)",
                (int)msg->call_id.length,
                msg->call_id.data);
            destroy_call(session);
            call_release(session);
        }
        return nullptr;
    }

    return session;
}

#include <stdio.h>
#include "orbit/config.h"

size_t sip_generate_response(
    struct sip_message const *restrict const req,
    int const status_code,
    char const *restrict const reason_phrase,
    struct string_view const sdp_body,
    char *restrict const buffer,
    size_t const max_len) {
    if (req == nullptr || buffer == nullptr || max_len == 0)
        return 0;

    int const written = snprintf(
        buffer,
        max_len,
        "SIP/2.0 %d %s\r\n"
        "Via: %.*s\r\n"
        "From: %.*s\r\n"
        "To: %.*s;tag=wsrtp-1234\r\n"
        "Call-ID: %.*s\r\n"
        "CSeq: %.*s\r\n"
        "Contact: <sip:orbit@%s>\r\n"
        "Allow: INVITE, ACK, CANCEL, OPTIONS, BYE\r\n"
        "Accept: application/sdp\r\n"
        "Supported: timer\r\n"
        "Content-Length: %zu\r\n"
        "%s"
        "\r\n",
        status_code,
        reason_phrase,
        (int)req->via.length,
        req->via.data,
        (int)req->from_tag.length,
        req->from_tag.data,
        (int)req->to_tag.length,
        req->to_tag.data,
        (int)req->call_id.length,
        req->call_id.data,
        (int)req->cseq.length,
        req->cseq.data,
        g_config.sip_external_addr ? g_config.sip_external_addr : "127.0.0.1",
        sdp_body.length,
        sdp_body.length > 0 ? "Content-Type: application/sdp\r\n" : "");

    if (written < 0 || (size_t)written >= max_len)
        return 0;

    size_t const hdr_len = (size_t)written;

    if (sdp_body.length > 0) {
        if (hdr_len + sdp_body.length >= max_len)
            return 0;
        memcpy(buffer + hdr_len, sdp_body.data, sdp_body.length);
        return hdr_len + sdp_body.length;
    }

    return hdr_len;
}

void sip_router_process_recv(struct io_event_ctx *restrict const ctx, size_t const len) {
    struct sip_message msg = {};
    struct string_view raw = {.data = (char *)ctx->buffer, .length = len};

    if (__builtin_expect(sip_parse_message(raw, &msg), 1)) {
        struct call_session *session = nullptr;

        if (msg.verb == SIP_VERB_INVITE && server_is_draining()) {
            LOGINF("SIP Router: Refusing INVITE (Server Draining)");
        } else {
            session = sip_router_process(&msg);
        }

        // Always generate a response for INVITE, OPTIONS, and BYE
        if (msg.verb == SIP_VERB_INVITE || msg.verb == SIP_VERB_OPTIONS || msg.verb == SIP_VERB_BYE)
        {
            // Re-arm send context with sender's address
            sip_send_ctx.msg.msg_namelen = ctx->msg.msg_namelen;
            memcpy(&sip_send_ctx.remote_addr, &ctx->remote_addr, sizeof(struct sockaddr_in));

            char               sdp_buf[1024] = {0};
            struct string_view sdp_view      = {0};
            int                status        = 200;
            char const        *reason        = "OK";

            if (msg.verb == SIP_VERB_INVITE && server_is_draining()) {
                status = 503;
                reason = "Service Unavailable";
            } else if (msg.verb == SIP_VERB_INVITE && session != nullptr) {
                // If it's an invite, ensure we allocate an RTP port!
                if (session->local_sdp.port == 0) {
                    uint16_t port   = 0;
                    session->rtp_fd = rtp_engine_allocate_port(session, &port);
                    if (session->rtp_fd < 0) {
                        LOGERR(
                            "SIP Router: Failed to allocate RTP port for Call-ID %.*s",
                            (int)msg.call_id.length,
                            msg.call_id.data);
                        status = 500;
                        reason = "Server Internal Error";
                    } else {
                        session->local_sdp.port = port;
                        int sdp_len   = (int)sdp_generate_reply(session, sdp_buf, sizeof(sdp_buf));
                        sdp_view.data = sdp_buf;
                        sdp_view.length = (size_t)sdp_len;
                    }
                }
            } else if (msg.verb == SIP_VERB_INVITE) {
                status = 486;
                reason = "Busy Here";
            }

            size_t const resp_len = sip_generate_response(
                &msg,
                status,
                reason,
                sdp_view,
                (char *)sip_send_buffer,
                sizeof(sip_send_buffer));
            if (resp_len > 0) {
                sip_send_ctx.iov.iov_len = resp_len;
                if (server_submit_sendmsg(&sip_send_ctx) < 0) {
                    LOGERR("SIP Router: Failed to submit sendmsg for SIP response");
                }
            } else {
                LOGERR("SIP Router: Failed to generate SIP response");
            }
        }
        if (session != nullptr) {
            call_release(session);
        }
    } else {
        LOGERR("SIP Router: Failed to parse incoming SIP message");
    }

    // Re-arm receive
    ctx->msg.msg_namelen = sizeof(struct sockaddr_in);
    server_submit_recv(ctx);
}

void sip_router_process_send(struct io_event_ctx *restrict const ctx) {
    // Nothing to do but we could re-arm if we had a queue.
    // Here we just ignore completion of send.
    (void)ctx;
}

void sip_router_cleanup(void) {
    if (sip_fd >= 0) {
        close(sip_fd);
        sip_fd = -1;
    }
    auto old_arr = __atomic_exchange_n(&active_calls, nullptr, __ATOMIC_ACQ_REL);
    if (old_arr != nullptr) {
        free(old_arr);
    }
    if (call_pool != nullptr) {
        pool_destroy(call_pool);
        call_pool = nullptr;
    }
}
