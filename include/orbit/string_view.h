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
#include <stdint.h>

/**
 * @brief Represents a zero-copy string slice.
 *
 * References an external, non-null-terminated string buffer with an explicit length,
 * avoiding allocations during string parsing.
 */
struct string_view {
    char const *restrict data; /**< Pointer to the start of the character sequence. */
    size_t length;             /**< Length of the string slice in bytes. */
};

// Use for compound literal inline instantiation
// Example: process_data(SV("hello"));
#define SV(str) ((struct string_view){.data = (str), .length = __builtin_strlen(str)})

#define SV_LEN(str, len) ((struct string_view){.data = (str), .length = (len)})

// Use for struct field initialization or constexpr initializations
// Example: constexpr struct string_view view = SV_INIT("hello");
#define SV_INIT(str) {.data = (str), .length = __builtin_strlen(str)}

#define SV_INIT_LEN(str, len) {.data = (str), .length = (len)}

/**
 * @brief Checks if a string view equals a null-terminated string.
 *
 * Performs a zero-copy comparison between the string view context and
 * the provided C-style string.
 *
 * @param a The string view.
 * @param b The null-terminated C string to compare.
 * @return true if the contents and length match exactly, or false otherwise.
 */
static inline bool sv_equals(struct string_view const a, char const *restrict const b) {
    if (a.data == nullptr || b == nullptr) {
        return false;
    }
    auto const len = __builtin_strlen(b);
    if (a.length != len) {
        return false;
    }
    return __builtin_memcmp(a.data, b, len) == 0;
}

/**
 * @brief Finds the first occurrence of a needle substring in a string view.
 *
 * @param sv The source string view to search.
 * @param needle The null-terminated C string substring to find.
 * @return A pointer within the string view where the needle starts, or nullptr if not found.
 */
static inline char const *sv_find(struct string_view const sv, char const *restrict const needle) {
    if (sv.data == nullptr || needle == nullptr) {
        return nullptr;
    }
    auto const needle_len = __builtin_strlen(needle);
    if (needle_len > sv.length) {
        return nullptr;
    }
    if (needle_len == 0) {
        return sv.data;
    }

    for (size_t i = 0; i <= sv.length - needle_len; ++i) {
        if (__builtin_memcmp(sv.data + i, needle, needle_len) == 0) {
            return sv.data + i;
        }
    }
    return nullptr;
}

