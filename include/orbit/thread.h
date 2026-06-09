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

#include <stdbool.h>
#include <stddef.h>

struct worker_ctx {
    int worker_id;
    int core_id;
    int event_fd; // IPC signal from supervisor
};

// Spawn all worker threads and pin them to cores
int worker_pool_init(void);

// Wait for all workers to shut down
void worker_pool_cleanup(void);

// Signal all workers to gracefully shut down
void worker_pool_stop_all(void);

// Get the current worker's ID
int worker_get_id(void);

// Get the total number of workers
int worker_get_count(void);
