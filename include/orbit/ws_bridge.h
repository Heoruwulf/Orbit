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

int  ws_bridge_init(void);
void ws_bridge_cleanup(void);

void ws_bridge_process_accept(struct io_event_ctx *restrict const ctx, int const res);
void ws_bridge_process_recv(struct io_event_ctx *restrict const ctx, int const res);
void ws_bridge_process_send(struct io_event_ctx *restrict const ctx, int const res);

void ws_bridge_send_binary(struct call_session *sip_call, uint8_t const *data, size_t len);
void ws_bridge_close(struct call_session *sip_call);
