#define BITE_NO_MAIN
#include "../src/bite.c"

static size_t checked;

static void check(const uint8_t *data, size_t size) {
    /* EXP-0083: the SA-IS dispatcher must reproduce prefix doubling exactly. */
    uint32_t doubling_primary = 0, primary = 0;
    Buffer doubling = bwt_transform_doubling(data, size, &doubling_primary);
    Buffer transformed = bwt_transform(data, size, &primary);
    if (primary != doubling_primary || memcmp(doubling.data, transformed.data, size))
        fail("BWT sorter mismatch");
    free(doubling.data); free(transformed.data);
    Buffer coded = bwt_encode(data, size);
    Buffer decoded = bwt_decode(coded.data, coded.size, size);
    if (decoded.size != size || memcmp(data, decoded.data, size))
        fail("BWT round-trip mismatch");
    free(coded.data); free(decoded.data);
    ++checked;
}

int main(void) {
    uint8_t data[131073];
    /* Exhaustive small binary strings include every periodic/tie arrangement. */
    for (unsigned size = 1; size <= 9; ++size) {
        for (unsigned value = 0; value < (1u << size); ++value) {
            for (unsigned i = 0; i < size; ++i) data[i] = (value >> i) & 1u;
            check(data, size);
        }
    }
    const size_t sizes[] = {127, 128, 129, 255, 256, 257, 16383, 16384,
                            16385, 65535, 65536, 65537, 131073};
    uint32_t state = 123456789u;
    for (size_t j = 0; j < sizeof(sizes) / sizeof(sizes[0]); ++j) {
        size_t size = sizes[j];
        for (unsigned kind = 0; kind < 4; ++kind) {
            for (size_t i = 0; i < size; ++i) {
                state ^= state << 13u; state ^= state >> 17u; state ^= state << 5u;
                data[i] = kind == 0 ? 255 : kind == 1 ? (uint8_t)(i % 7u) :
                          kind == 2 ? (uint8_t)i : (uint8_t)state;
            }
            /* Direct calls force the compressed path even when raw would win. */
            check(data, size);
        }
    }
    printf("%zu forced BWT round trips passed\n", checked);
    return 0;
}
