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

/**
 * @brief Represents a single packet slot within the jitter buffer.
 */
struct jitter_node {
    void *restrict buffer; /**< Pointer to the buffered raw RTP packet memory. */
    size_t   length;       /**< Size of the raw RTP packet in bytes. */
    uint16_t seq_num;      /**< RTP sequence number extracted from the packet header. */
    bool     is_valid;     /**< Flag indicating if the slot contains an unpopped packet. */
};

/**
 * @brief Cacheline-aligned jitter buffer for sorting out-of-order RTP packets.
 *
 * Implements a ring buffer of dynamic nodes with force advancement tracking to handle
 * gaps, duplicates, and late packets gracefully.
 */
struct jitter_buffer {
    alignas(64) struct jitter_node
        slots[JITTER_BUFFER_SIZE]; /**< Fixed-size array of buffering packet slots. */
    uint16_t next_pop_seq;         /**< The sequence number of the next expected packet to pop. */
    bool     has_started;          /**< True if the buffer has received its initial packet. */
    bool     has_popped;           /**< True if the caller has started popping packets. */
};

/**
 * @brief Initializes the jitter buffer structure to default empty state.
 *
 * Zeroes out the slots and status variables.
 *
 * @param jb The jitter buffer structure pointer to initialize.
 */
void jitter_buffer_init(struct jitter_buffer *restrict const jb);

/**
 * @brief Pushes a raw received RTP packet into the jitter buffer.
 *
 * Extracts the packet sequence number, checks if it is within the active buffering
 * window (dropping late or excessively future packets), and stores the buffer in the
 * appropriate ring buffer index slot.
 *
 * @param jb The jitter buffer structure pointer.
 * @param buffer A pointer to the raw RTP packet memory.
 * @param length The total size of the RTP packet in bytes.
 * @return true if successfully queued, or false if invalid, late, duplicate, or out of window.
 */
bool jitter_buffer_push(
    struct jitter_buffer *restrict const jb,
    void *restrict const buffer,
    size_t const length);

/**
 * @brief Pops the next expected RTP packet in sequence from the jitter buffer.
 *
 * Retrieves the packet matching next_pop_seq. If the packet is not available,
 * it returns false, indicating a packet loss/gap (where the caller should generate silence).
 * Forces advancing the sequence number on return.
 *
 * @param jb The jitter buffer structure pointer.
 * @param out_buffer Output pointer to store the retrieved packet buffer pointer.
 * @param out_length Output pointer to store the length of the retrieved packet.
 * @return true if the expected packet was found and returned, or false if missing/not yet buffered.
 */
bool jitter_buffer_pop(
    struct jitter_buffer *restrict const jb,
    void **restrict out_buffer,
    size_t *restrict out_length);
