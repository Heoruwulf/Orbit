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

struct pool_config {
    size_t object_size;
    size_t count;
};

mem_pool_t *pool_create(struct pool_config config);
void       *pool_alloc(mem_pool_t *pool);
void        pool_free(mem_pool_t *pool, void *ptr);
void        pool_destroy(mem_pool_t *pool);
