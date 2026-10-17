/* Experimental cyclic BWT, move-to-front and zero-run coding.
 * Included after the arithmetic primitives in bite.c. No external codec.
 * Aperiodic blocks sort rotations as suffixes of the doubled block with SA-IS
 * (EXP-0083): linear time. Exact repetitions of a shorter string have equal
 * rotations, so they keep prefix doubling, whose tie order defines the stream.
 */

/* SA-IS (Nong, Zhang and Chan) over s[0..n), where s[n-1] == 0 is a unique
 * smallest sentinel and every value is below k. */
static void sais_buckets(const int32_t *s, int32_t n, int32_t k, int32_t *bucket, int end) {
    memset(bucket, 0, (size_t)k * sizeof(*bucket));
    for (int32_t i = 0; i < n; ++i) ++bucket[s[i]];
    int32_t sum = 0;
    for (int32_t c = 0; c < k; ++c) {
        sum += bucket[c];
        bucket[c] = end ? sum : sum - bucket[c];
    }
}

static void sais_induce(const int32_t *s, const uint8_t *stype, int32_t *sa, int32_t n,
                        int32_t k, int32_t *bucket) {
    sais_buckets(s, n, k, bucket, 0);
    for (int32_t i = 0; i < n; ++i) {
        int32_t j = sa[i] - 1;
        if (sa[i] > 0 && !stype[j]) sa[bucket[s[j]]++] = j;
    }
    sais_buckets(s, n, k, bucket, 1);
    for (int32_t i = n; i-- > 0;) {
        int32_t j = sa[i] - 1;
        if (sa[i] > 0 && stype[j]) sa[--bucket[s[j]]] = j;
    }
}

static void sais_core(const int32_t *s, int32_t *sa, int32_t n, int32_t k) {
    uint8_t *stype = malloc((size_t)n);
    int32_t *bucket = malloc((size_t)k * sizeof(*bucket));
    if (!stype || !bucket) fail("out of memory");
    stype[n - 1] = 1;
    for (int32_t i = n - 1; i-- > 0;)
        stype[i] = s[i] < s[i + 1] || (s[i] == s[i + 1] && stype[i + 1]);
#define SAIS_LMS(i) ((i) > 0 && stype[i] && !stype[(i) - 1])
    sais_buckets(s, n, k, bucket, 1);
    for (int32_t i = 0; i < n; ++i) sa[i] = -1;
    for (int32_t i = 1; i < n; ++i)
        if (SAIS_LMS(i)) sa[--bucket[s[i]]] = i;
    sais_induce(s, stype, sa, n, k, bucket);

    /* Name the sorted LMS substrings; positions are at least two apart. */
    int32_t lms_count = 0;
    for (int32_t i = 0; i < n; ++i)
        if (SAIS_LMS(sa[i])) sa[lms_count++] = sa[i];
    for (int32_t i = lms_count; i < n; ++i) sa[i] = -1;
    int32_t names = 0, previous = -1;
    for (int32_t i = 0; i < lms_count; ++i) {
        int32_t position = sa[i], differs = previous < 0;
        for (int32_t d = 0; !differs; ++d) {
            if (s[position + d] != s[previous + d] ||
                stype[position + d] != stype[previous + d])
                differs = 1;
            else if (d > 0 && (SAIS_LMS(position + d) || SAIS_LMS(previous + d)))
                break;
        }
        if (differs) { ++names; previous = position; }
        sa[lms_count + position / 2] = names - 1;
    }
    for (int32_t i = n, j = n; i-- > lms_count;)
        if (sa[i] >= 0) sa[--j] = sa[i];

    int32_t *reduced = sa + n - lms_count, *reduced_sa = sa;
    if (names < lms_count) {
        sais_core(reduced, reduced_sa, lms_count, names);
    } else {
        for (int32_t i = 0; i < lms_count; ++i) reduced_sa[reduced[i]] = i;
    }
    for (int32_t i = 1, j = 0; i < n; ++i)
        if (SAIS_LMS(i)) reduced[j++] = i;
    for (int32_t i = 0; i < lms_count; ++i) reduced_sa[i] = reduced[reduced_sa[i]];
    for (int32_t i = lms_count; i < n; ++i) sa[i] = -1;
    sais_buckets(s, n, k, bucket, 1);
    for (int32_t i = lms_count; i-- > 0;) {
        int32_t j = sa[i];
        sa[i] = -1;
        sa[--bucket[s[j]]] = j;
    }
    sais_induce(s, stype, sa, n, k, bucket);
#undef SAIS_LMS
    free(bucket); free(stype);
}

/* Smallest period dividing size, or size when the block is aperiodic. */
static size_t bwt_period(const uint8_t *input, size_t size) {
    uint32_t *border = malloc(size * sizeof(*border));
    if (!border) fail("out of memory");
    border[0] = 0;
    for (size_t i = 1; i < size; ++i) {
        uint32_t k = border[i - 1u];
        while (k && input[i] != input[k]) k = border[k - 1u];
        border[i] = k + (input[i] == input[k]);
    }
    size_t period = size - border[size - 1u];
    free(border);
    return size % period ? size : period;
}

static Buffer bwt_transform_sais(const uint8_t *input, size_t size, uint32_t *primary) {
    int32_t n = (int32_t)size, m = 2 * n + 1;
    int32_t *text = malloc((size_t)m * sizeof(*text));
    int32_t *sa = malloc((size_t)m * sizeof(*sa));
    if (!text || !sa) fail("out of memory");
    for (int32_t i = 0; i < n; ++i) text[i] = text[i + n] = input[i] + 1;
    text[2 * n] = 0;
    sais_core(text, sa, m, 257);
    free(text);
    Buffer last = {0};
    reserve(&last, size);
    for (int32_t i = 1, row = 0; i < m; ++i) {
        int32_t start = sa[i];
        if (start >= n) continue;
        if (!start) *primary = (uint32_t)row;
        last.data[row++] = input[start ? start - 1 : n - 1];
    }
    last.size = size;
    free(sa);
    return last;
}

static Buffer bwt_transform_doubling(const uint8_t *input, size_t size, uint32_t *primary) {
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

static Buffer bwt_transform(const uint8_t *input, size_t size, uint32_t *primary) {
    if (!size || size > WHOLE_BLOCK_SIZE) fail("invalid BWT block size");
    return bwt_period(input, size) == size
        ? bwt_transform_sais(input, size, primary)
        : bwt_transform_doubling(input, size, primary);
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
