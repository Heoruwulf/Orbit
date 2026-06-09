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
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

constexpr size_t JITTER_BUFFER_SIZE = 64;

struct jitter_node {
    void *restrict buffer;
    size_t   length;
    uint16_t seq_num;
    bool     is_valid;
};

struct jitter_buffer {
    alignas(64) struct jitter_node slots[JITTER_BUFFER_SIZE];
    uint16_t next_pop_seq;
    bool     has_started;
    bool     has_popped;
};

// Initializes the jitter buffer struct (zeroes it out)
void jitter_buffer_init(struct jitter_buffer *restrict const jb);

// Pushes a raw RTP packet payload into the jitter buffer.
// If it's too old or a duplicate, returns false.
// If it succeeds, the jitter buffer takes ownership of the buffer pointer.
bool jitter_buffer_push(
    struct jitter_buffer *restrict const jb,
    void *restrict const buffer,
    size_t const length);

// Pops the next expected RTP packet in sequence.
// If the packet is missing, returns false (and we should generate silence).
// The caller is responsible for freeing the buffer back to the pool.
bool jitter_buffer_pop(
    struct jitter_buffer *restrict const jb,
    void **restrict out_buffer,
    size_t *restrict out_length);
