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

#include "orbit/config.h"
#include "orbit/sip_router.h"

typedef int (*event_init_fn)(void);
typedef int (*event_publish_fn)(char const *restrict const payload, size_t const len);
typedef void (*event_cleanup_fn)(void);

struct event_provider {
    char const      *name;
    event_init_fn    init;
    event_publish_fn publish;
    event_cleanup_fn cleanup;
};

/**
 * @brief Initializes the global event subsystem (queues and background thread).
 * Must be called by the supervisor thread before worker threads are spawned.
 *
 * @return 0 on success, or -1 on failure.
 */
int event_global_init(void);

/**
 * @brief Cleans up the global event subsystem resources.
 * Must be called by the supervisor thread after workers have stopped.
 */
void event_global_cleanup(void);

/**
 * @brief Initializes the UDP event publisher socket and target address.
 *
 * Sets up a non-blocking UDP socket and parses the configured event address
 * and port if both are specified in the global configuration.
 */
void event_init(void);

/**
 * @brief Publishes a JSON-serialized call answered event.
 *
 * Serializes the SIP call details (Call-ID, From, To tags, custom headers)
 * and the generated WebSocket URL into a JSON payload using a zero-allocation
 * memory pool, then pushes it to the thread-local event queue or sends it directly.
 *
 * @param msg The parsed incoming SIP message triggering the event.
 * @param internal_id The unique internal UUID string view assigned to the call.
 */
void event_publish_call_answered(
    struct sip_message const *restrict const msg,
    struct string_view const internal_id);
