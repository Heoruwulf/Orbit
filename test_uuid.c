#include <stdio.h>
#include <stdint.h>

static uint64_t prng_state = 0x1234567890abcdef;
static uint64_t xorshift64(void) {
    uint64_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return prng_state = x;
}

int main() {
    for (int i=0; i<3; i++) {
        uint64_t r1 = xorshift64();
        uint64_t r2 = xorshift64();
        uint8_t *b1 = (uint8_t *)&r1;
        uint8_t *b2 = (uint8_t *)&r2;
        char out[37];
        snprintf(
            out,
            37,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            b1[0], b1[1], b1[2], b1[3], b1[4], b1[5],
            (b1[6] & 0x0f) | 0x40, b1[7],
            (b2[0] & 0x3f) | 0x80, b2[1], b2[2], b2[3], b2[4], b2[5], b2[6], b2[7]);
        printf("%s\n", out);
    }
    return 0;
}
