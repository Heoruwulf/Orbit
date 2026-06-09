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

typedef enum event_type {
    EVENT_TYPE_SIGNAL    = 1,
    EVENT_TYPE_RTP_RECV  = 2,
    EVENT_TYPE_RTP_SEND  = 3,
    EVENT_TYPE_WS_ACCEPT = 4,
    EVENT_TYPE_WS_RECV   = 5,
    EVENT_TYPE_WS_SEND   = 6,
    EVENT_TYPE_SIP_RECV  = 7,
    EVENT_TYPE_SIP_SEND  = 8
} event_type_t;

struct io_event_ctx {
    event_type_t type;
    int          fd;
    void *restrict buffer;
    size_t               length;
    struct call_session *session;
    struct msghdr        msg;
    struct iovec         iov;
    struct sockaddr_in   remote_addr;
};

int  server_init(void);
void server_poll_once(void);
void server_run(int event_fd);
void server_stop(void);
void server_cleanup(void);
bool server_is_draining(void);

int server_submit_recv(struct io_event_ctx *restrict const ctx);
int server_submit_sendmsg(struct io_event_ctx *restrict const ctx);
int server_submit_accept(struct io_event_ctx *restrict const ctx);
int server_submit_send(struct io_event_ctx *restrict const ctx);
