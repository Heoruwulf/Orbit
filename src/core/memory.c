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
#include "orbit/memory.h"
#include <stdlib.h>

struct pool_node {
    struct pool_node *next;
};

struct mem_pool {
    size_t            object_size;
    size_t            count;
    void             *memory_block;
    struct pool_node *free_list;
};

mem_pool_t *pool_create(struct pool_config const config) {
    auto actual_size = config.object_size;
    if (actual_size < sizeof(struct pool_node)) {
        actual_size = sizeof(struct pool_node);
    }
    // Align object size to 64 bytes (cache line)
    actual_size = (actual_size + 63) & ~((size_t)63);

    auto pool = (mem_pool_t *)malloc(sizeof(mem_pool_t));
    if (pool == nullptr) {
        return nullptr;
    }

    pool->object_size  = actual_size;
    pool->count        = config.count;
    pool->memory_block = aligned_alloc(64, actual_size * config.count);

    if (pool->memory_block == nullptr) {
        free(pool);
        return nullptr;
    }

    pool->free_list = nullptr;
    auto ptr        = (char *)pool->memory_block;

    // Prefault memory block by zeroing it, avoiding minor page faults during hot path
    __builtin_memset(pool->memory_block, 0, actual_size * config.count);

    // Initialize free list
    for (size_t i = 0; i < config.count; ++i) {
        auto const node = (struct pool_node *)(ptr + (i * actual_size));
        node->next      = pool->free_list;
        pool->free_list = node;
    }

    return pool;
}

void *pool_alloc(mem_pool_t *const pool) {
    if (pool == nullptr || pool->free_list == nullptr) {
        return nullptr;
    }

    auto const node = pool->free_list;
    pool->free_list = node->next;
    return node;
}

void pool_free(mem_pool_t *const pool, void *const ptr) {
    if (pool == nullptr || ptr == nullptr) {
        return;
    }

    auto const node = (struct pool_node *)ptr;
    node->next      = pool->free_list;
    pool->free_list = node;
}

void pool_destroy(mem_pool_t *const pool) {
    if (pool == nullptr) {
        return;
    }

    if (pool->memory_block != nullptr) {
        free(pool->memory_block);
    }

    free(pool);
}
