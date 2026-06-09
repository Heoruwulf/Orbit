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
#include "orbit/rtp_jitter.h"

static inline uint16_t parse_seq_num(void const *restrict const buffer, size_t const length) {
    if (length < 12)
        return 0;
    uint8_t const *const bytes = (uint8_t const *)buffer;
    return (uint16_t)((bytes[2] << 8) | bytes[3]);
}

void jitter_buffer_init(struct jitter_buffer *restrict const jb) {
    if (jb == nullptr)
        return;
    *jb = (struct jitter_buffer){};
}

bool jitter_buffer_push(
    struct jitter_buffer *restrict const jb,
    void *restrict const buffer,
    size_t const length) {
    if (jb == nullptr || buffer == nullptr || length < 12)
        return false;

    auto const seq_num = parse_seq_num(buffer, length);

    if (!jb->has_started) {
        jb->has_started  = true;
        jb->next_pop_seq = seq_num;
    } else {
        int16_t const diff = (int16_t)(seq_num - jb->next_pop_seq);
        if (diff < 0) {
            if (!jb->has_popped && diff >= -((int)JITTER_BUFFER_SIZE)) {
                // We are still buffering, we can move the start window back!
                jb->next_pop_seq = seq_num;
            } else {
                return false; // Late packet, drop
            }
        } else if ((size_t)diff >= JITTER_BUFFER_SIZE) {
            return false; // Too far in future
        }
    }

    size_t const index = seq_num % JITTER_BUFFER_SIZE;

    if (jb->slots[index].is_valid) {
        return false;
    }

    jb->slots[index].buffer   = buffer;
    jb->slots[index].length   = length;
    jb->slots[index].seq_num  = seq_num;
    jb->slots[index].is_valid = true;

    return true;
}

bool jitter_buffer_pop(
    struct jitter_buffer *restrict const jb,
    void **restrict out_buffer,
    size_t *restrict out_length) {
    if (jb == nullptr || out_buffer == nullptr || out_length == nullptr)
        return false;
    if (!jb->has_started)
        return false;

    jb->has_popped     = true;
    size_t const index = jb->next_pop_seq % JITTER_BUFFER_SIZE;

    if (jb->slots[index].is_valid && jb->slots[index].seq_num == jb->next_pop_seq) {
        *out_buffer = jb->slots[index].buffer;
        *out_length = jb->slots[index].length;

        jb->slots[index].is_valid = false;
        jb->slots[index].buffer   = nullptr;

        jb->next_pop_seq++;
        return true;
    }

    // Missing packet. Force advance.
    jb->next_pop_seq++;
    return false;
}
