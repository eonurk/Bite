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
    /* Mode 26 shares the transform; check its context-mixed coder too. */
    coded = bwt2_encode(data, size);
    decoded = bwt2_decode(coded.data, coded.size, size);
    if (decoded.size != size || memcmp(data, decoded.data, size))
        fail("mode-26 BWT round-trip mismatch");
    free(coded.data); free(decoded.data);
    coded = bwt3_encode(data, size);
    decoded = bwt3_decode(coded.data, coded.size, size);
    if (decoded.size != size || memcmp(data, decoded.data, size))
        fail("mode-29 BWT round-trip mismatch");
    free(coded.data); free(decoded.data);
    ++checked;
}

/* Fibonacci frequencies need codes longer than BWT3_MAX_LENGTH before limiting. */
static void check_code_lengths(void) {
    uint32_t frequency[256] = {0}, a = 1, b = 1;
    uint8_t length[256];
    for (unsigned c = 0; c < 40; ++c) { frequency[c * 5u] = a; uint32_t t = a + b; a = b; b = t; }
    bwt3_code_lengths(frequency, length);
    Bwt3Model *m = bwt3_model_new(1);
    unsigned longest = 0;
    for (unsigned c = 0; c < 256; ++c) {
        if ((length[c] != 0) != (frequency[c] != 0)) fail("mode-29 code misses a literal");
        if (length[c] > longest) longest = length[c];
    }
    if (longest > BWT3_MAX_LENGTH || !bwt3_build_tree(m, length) || m->leaves != 40)
        fail("mode-29 length-limited code is not complete");
    memset(frequency, 0, sizeof(frequency));
    frequency[255] = 9;
    bwt3_code_lengths(frequency, length);
    if (length[255] != 1 || length[254] != 1 || !bwt3_build_tree(m, length) || m->leaves != 2)
        fail("mode-29 single-literal code needs a sibling");
    length[254] = 2;
    if (bwt3_build_tree(m, length)) fail("mode-29 accepted an incomplete code");
    bwt3_model_free(m);
}

int main(void) {
    static uint8_t data[262144];
    check_code_lengths();
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
    /* Skewed literal frequencies exercise length limiting end to end. */
    uint32_t a = 1, b = 1;
    size_t size = 0;
    for (unsigned c = 0; c < 26 && size + a <= sizeof(data); ++c) {
        memset(data + size, (int)(c * 9u + 1u), a);
        size += a;
        uint32_t t = a + b; a = b; b = t;
    }
    for (size_t i = size; i > 1; --i) {
        state ^= state << 13u; state ^= state >> 17u; state ^= state << 5u;
        size_t j = state % i;
        uint8_t t = data[i - 1]; data[i - 1] = data[j]; data[j] = t;
    }
    check(data, size);
    printf("%zu forced mode-25, mode-26 and mode-29 BWT round trips passed\n", checked);
    return 0;
}
