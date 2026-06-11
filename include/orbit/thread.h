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

/**
 * @brief Context structure containing parameters for a worker thread.
 */
struct worker_ctx {
    int worker_id; /**< The unique logical index assigned to the worker. */
    int core_id;   /**< The target CPU core index to pin this worker thread's affinity. */
    int event_fd; /**< eventfd descriptor used by supervisor to push IPC signals (e.g. shutdown). */
};

/**
 * @brief Spawns the worker threads and pins them to CPU cores.
 *
 * Allocates thread arrays, creates eventfd communication descriptors for each worker,
 * starts the worker loop (worker_thread_main), and applies CPU affinity to bind
 * each worker thread to its specific core.
 *
 * @return 0 on success, or -1 on initialization or thread creation failure.
 */
int worker_pool_init(void);

/**
 * @brief Waits for all worker threads to stop and cleans up their resources.
 *
 * Joins each thread and closes the corresponding eventfd.
 */
void worker_pool_cleanup(void);

/**
 * @brief Signals all worker threads in the pool to shut down.
 *
 * Writes to the eventfd of each worker to trigger thread loop exit.
 */
void worker_pool_stop_all(void);

/**
 * @brief Retrieves the worker ID of the calling thread.
 *
 * @return The integer ID of the current thread, or -1 if called by supervisor/non-worker thread.
 */
int worker_get_id(void);

/**
 * @brief Retrieves the total count of workers in the pool.
 *
 * @return Total number of spawned worker threads.
 */
int worker_get_count(void);
