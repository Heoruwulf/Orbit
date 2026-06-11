#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct string_view {
    char const *restrict data;
    size_t length;
};

// Use for compound literal inline instantiation
// Example: process_data(SV("hello"));
#define SV(str) ((struct string_view){.data = (str), .length = __builtin_strlen(str)})

#define SV_LEN(str, len) ((struct string_view){.data = (str), .length = (len)})

// Use for struct field initialization or constexpr initializations
// Example: constexpr struct string_view view = SV_INIT("hello");
#define SV_INIT(str) {.data = (str), .length = __builtin_strlen(str)}

#define SV_INIT_LEN(str, len) {.data = (str), .length = (len)}

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
