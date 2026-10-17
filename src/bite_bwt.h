/* Experimental cyclic BWT, move-to-front and zero-run coding.
 * Included after the arithmetic primitives in bite.c. No external codec.
 * Prefix doubling uses counting sorts: O(n log n) time, O(n) memory,
 * including periodic and all-equal inputs (no quadratic comparator).
 */
static Buffer bwt_transform(const uint8_t *input, size_t size, uint32_t *primary) {
    Buffer last = {0};
    if (!size || size > WHOLE_BLOCK_SIZE) fail("invalid BWT block size");
    uint32_t n = (uint32_t)size;
    uint32_t *order = malloc(size * sizeof(*order));
    uint32_t *rank = malloc(size * sizeof(*rank));
    uint32_t *next = malloc(size * sizeof(*next));
    uint32_t *work = malloc(size * sizeof(*work));
    size_t count_size = size > 256u ? size : 256u;
    uint32_t *count = calloc(count_size, sizeof(*count));
    if (!order || !rank || !next || !work || !count) fail("out of memory");
    for (uint32_t i = 0; i < n; ++i) ++count[input[i]];
    uint32_t sum = 0;
    for (unsigned c = 0; c < 256; ++c) {
        uint32_t frequency = count[c]; count[c] = sum; sum += frequency;
    }
    for (uint32_t i = 0; i < n; ++i) order[count[input[i]]++] = i;
    uint32_t classes = 1;
    rank[order[0]] = 0;
    for (uint32_t i = 1; i < n; ++i) {
        if (input[order[i]] != input[order[i - 1u]]) ++classes;
        rank[order[i]] = classes - 1u;
    }
    for (uint32_t width = 1; width < n && classes < n; width *= 2u) {
        memset(count, 0, classes * sizeof(*count));
        for (uint32_t i = 0; i < n; ++i) {
            work[i] = order[i] >= width ? order[i] - width : order[i] + n - width;
            ++count[rank[work[i]]];
        }
        sum = 0;
        for (uint32_t c = 0; c < classes; ++c) {
            uint32_t frequency = count[c]; count[c] = sum; sum += frequency;
        }
        for (uint32_t i = 0; i < n; ++i) order[count[rank[work[i]]]++] = work[i];
        uint32_t new_classes = 1;
        next[order[0]] = 0;
        for (uint32_t i = 1; i < n; ++i) {
            uint32_t a = order[i], b = order[i - 1u];
            uint32_t a2 = a + width < n ? a + width : a + width - n;
            uint32_t b2 = b + width < n ? b + width : b + width - n;
            if (rank[a] != rank[b] || rank[a2] != rank[b2]) ++new_classes;
            next[a] = new_classes - 1u;
        }
        uint32_t *swap = rank; rank = next; next = swap;
        classes = new_classes;
    }
    reserve(&last, size);
    for (uint32_t i = 0; i < n; ++i) {
        if (!order[i]) *primary = i;
        last.data[i] = input[order[i] ? order[i] - 1u : n - 1u];
    }
    last.size = size;
    free(count); free(work); free(next); free(rank); free(order);
    return last;
}

static void bwt_append_run(Buffer *tokens, size_t run) {
    if (!run) return;
    append_byte(tokens, 0);
    do {
        unsigned byte = run & 127u;
        run >>= 7u;
        append_byte(tokens, (uint8_t)(byte | (run ? 128u : 0u)));
    } while (run);
}

static Buffer bwt_encode(const uint8_t *input, size_t size) {
    uint32_t primary = 0;
    Buffer last = bwt_transform(input, size, &primary);
    uint8_t alphabet[256];
    for (unsigned i = 0; i < 256; ++i) alphabet[i] = (uint8_t)i;
    Buffer tokens = {0};
    size_t run = 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned rank = 0;
        while (alphabet[rank] != last.data[i]) ++rank;
        if (!rank) { ++run; continue; }
        bwt_append_run(&tokens, run); run = 0;
        append_byte(&tokens, (uint8_t)rank);
        uint8_t symbol = alphabet[rank];
        memmove(alphabet + 1, alphabet, rank);
        alphabet[0] = symbol;
    }
    bwt_append_run(&tokens, run);
    free(last.data);
    Buffer coded = adaptive_order1_arithmetic_encode(tokens.data, tokens.size);
    free(tokens.data);
    Buffer output = {0};
    append_u32le(&output, primary);
    append_bytes(&output, coded.data, coded.size);
    free(coded.data);
    return output;
}

static Buffer bwt_decode(const uint8_t *input, size_t size, size_t decoded_size) {
    if (size < 12 || !decoded_size || decoded_size > WHOLE_BLOCK_SIZE)
        fail("invalid BWT payload");
    uint32_t primary = read_u32le(input);
    if (primary >= decoded_size) fail("invalid BWT primary index");
    /* A zero rank costs at most two tokens; reject oversized streams before allocation. */
    if (read_u32le(input + 4) > decoded_size * 2u)
        fail("invalid BWT token count");
    Buffer tokens = adaptive_order1_arithmetic_decode(input + 4, size - 4);
    Buffer last = {0};
    reserve(&last, decoded_size);
    uint8_t alphabet[256];
    for (unsigned i = 0; i < 256; ++i) alphabet[i] = (uint8_t)i;
    for (size_t i = 0; i < tokens.size; ++i) {
        unsigned rank = tokens.data[i];
        if (!rank) {
            uint32_t run = 0;
            unsigned shift = 0, byte;
            do {
                if (++i >= tokens.size || shift > 21u) fail("invalid BWT run");
                byte = tokens.data[i];
                run |= (uint32_t)(byte & 127u) << shift;
                shift += 7u;
            } while (byte & 128u);
            if (!run || run > decoded_size - last.size) fail("invalid BWT run size");
            memset(last.data + last.size, alphabet[0], run);
            last.size += run;
        } else {
            if (last.size == decoded_size) fail("excess BWT symbols");
            uint8_t symbol = alphabet[rank];
            last.data[last.size++] = symbol;
            memmove(alphabet + 1, alphabet, rank);
            alphabet[0] = symbol;
        }
    }
    free(tokens.data);
    if (last.size != decoded_size) fail("incomplete BWT block");
    uint32_t counts[256] = {0}, starts[256];
    for (size_t i = 0; i < decoded_size; ++i) ++counts[last.data[i]];
    uint32_t sum = 0;
    for (unsigned c = 0; c < 256; ++c) { starts[c] = sum; sum += counts[c]; }
    uint32_t *links = malloc(decoded_size * sizeof(*links));
    if (!links) fail("out of memory");
    for (size_t i = 0; i < decoded_size; ++i) links[i] = starts[last.data[i]]++;
    Buffer output = {0};
    reserve(&output, decoded_size);
    uint32_t row = primary;
    for (size_t i = decoded_size; i > 0; --i) {
        output.data[i - 1u] = last.data[row];
        row = links[row];
    }
    output.size = decoded_size;
    free(links); free(last.data);
    return output;
}

/* Standalone research preset, with the same framing and raw fallback as cmax. */
static Buffer compress_bwt_blocked(const uint8_t *input, size_t size) {
    Buffer output = {0};
    const uint8_t header[8] = {'B', 'I', 'T', 'E', 2, 0, 0, 0};
    append_bytes(&output, header, sizeof(header));
    append_u64le(&output, size);
    for (size_t pos = 0; pos < size;) {
        size_t block = size - pos;
        if (block > WHOLE_BLOCK_SIZE) block = WHOLE_BLOCK_SIZE;
        Buffer coded = bwt_encode(input + pos, block);
        int selected = coded.size < block;
        append_byte(&output, selected ? 25 : 0);
        append_u32le(&output, (uint32_t)block);
        append_u32le(&output, (uint32_t)(selected ? coded.size : block));
        append_bytes(&output, selected ? coded.data : input + pos,
                     selected ? coded.size : block);
        free(coded.data);
        pos += block;
    }
    return output;
}
