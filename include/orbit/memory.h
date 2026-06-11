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

#include <stddef.h>

typedef struct mem_pool mem_pool_t;

/**
 * @brief Memory pool configuration parameters.
 */
struct pool_config {
    size_t object_size; /**< Size of a single pre-allocated object node in bytes. */
    size_t count;       /**< Maximum capacity / number of object nodes in the pool. */
};

/**
 * @brief Creates a lock-free, zero-allocation memory pool.
 *
 * Allocates and aligns the underlying memory block to CPU cache lines (64 bytes),
 * prefaults the memory by zero-initializing it, and initializes the free list nodes.
 *
 * @param config Configuration specifying the object size and the pool count capacity.
 * @return A pointer to the created memory pool, or nullptr on failure.
 */
mem_pool_t *pool_create(struct pool_config config);

/**
 * @brief Allocates an object from the memory pool.
 *
 * Retrieves a pre-allocated object from the front of the pool's free list.
 *
 * @param pool A pointer to the memory pool.
 * @return A pointer to the allocated object, or nullptr if the pool is exhausted.
 */
void       *pool_alloc(mem_pool_t *pool);

/**
 * @brief Releases an object back to the memory pool.
 *
 * Appends the freed object back to the pool's free list.
 *
 * @param pool A pointer to the memory pool.
 * @param ptr A pointer to the object being returned to the pool.
 */
void        pool_free(mem_pool_t *pool, void *ptr);

/**
 * @brief Destroys the memory pool and releases all allocated memory.
 *
 * Deallocates the underlying cacheline-aligned memory block and the pool structure.
 *
 * @param pool A pointer to the memory pool to destroy.
 */
void        pool_destroy(mem_pool_t *pool);

