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

static Buffer bwt_inverse(Buffer last, uint32_t primary);

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
    return bwt_inverse(last, primary);
}

/* Consumes the last column; primary must be below its size. */
static Buffer bwt_inverse(Buffer last, uint32_t primary) {
    size_t decoded_size = last.size;
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

/* Mode 26: the same transform, then bitwise context mixing of the last column.
 * Each byte is 8 binary decisions; every decision mixes a fixed set of counter
 * predictions, so work is linear in the block size. Integer-only arithmetic
 * keeps encoder and decoder identical across platforms.
 */
#define BWT2_CONTEXTS 6
#define BWT2_INPUTS 17
#define BWT2_MIXERS 4
#define BWT2_SETS (256u + 256u + 16u * 256u + 16u)
#define BWT2_LIMIT 1023
#define BWT2_RANK_LIMIT 255

typedef struct {
    uint32_t *counters[BWT2_CONTEXTS];  /* 22-bit probability, 10-bit count */
    uint8_t *histories[BWT2_CONTEXTS];  /* marker bit plus up to 7 recent bits */
    uint16_t *fast[2];                  /* fixed-rate order-0 and order-1 */
    uint32_t indirect[BWT2_CONTEXTS][16 * 256];
    uint32_t ranks[2][16 * 8 * 2 * 2];
    int32_t weights[BWT2_SETS * BWT2_INPUTS];
    int32_t final_weights[256 * BWT2_MIXERS];
    uint16_t apm_order0[256 * 33];
    uint16_t apm_order1[65536 * 33];
    short stretch[4096];
    uint32_t bases[BWT2_CONTEXTS];
    unsigned table_bits, c1, c2, d2, d3;
    uint32_t run;
} Bwt2Model;

typedef struct {
    uint32_t low, high, code;
    Buffer *output;
    const uint8_t *input;
    size_t size, pos;
} Bwt2Coder;

static int bwt2_squash(int d) {
    static const int table[33] = {
        1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047,
        2549, 2994, 3348, 3607, 3785, 3901, 3975, 4024, 4050, 4068, 4079, 4085,
        4089, 4092, 4093, 4094};
    if (d > 2047) return 4095;
    if (d < -2047) return 1;
    int w = d & 127, i = (d >> 7) + 16;
    return (table[i] * (128 - w) + table[i + 1] * w + 64) >> 7;
}

static int bwt2_clamp(int64_t d) {
    return d > 2047 ? 2047 : d < -2047 ? -2047 : (int)d;
}

static void bwt2_counter_update(uint32_t *t, int bit, int limit) {
    uint32_t n = *t & 1023u;
    int32_t p = (int32_t)(*t >> 10);
    if (n < (uint32_t)limit) ++*t;
    else *t = (*t & 0xfffffc00u) | (uint32_t)limit;
    int64_t delta = (((int64_t)bit << 22) - p) >> 3;
    *t += (uint32_t)(delta * (16384 / ((int)n + (int)n + 3))) & 0xfffffc00u;
}

static unsigned bwt2_bucket(uint32_t run) {
    static const uint32_t bounds[14] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 64, 128, 256, 1024};
    unsigned bucket = 0;
    while (bucket < 14 && run >= bounds[bucket]) ++bucket;
    return bucket;
}

static uint32_t bwt2_hash(uint32_t seed, uint32_t value) {
    uint32_t h = seed * 0x9E3779B1u ^ (value + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}

static Bwt2Model *bwt2_model_new(size_t block_size) {
    Bwt2Model *m = calloc(1, sizeof(*m));
    if (!m) fail("out of memory");
    m->table_bits = 12;
    while (m->table_bits < 20 && ((size_t)1 << m->table_bits) < block_size * 8u)
        ++m->table_bits;
    size_t slots = (size_t)1 << m->table_bits;
    for (unsigned j = 0; j < BWT2_CONTEXTS; ++j) {
        size_t count = j ? slots : 256u;
        m->counters[j] = malloc(count * sizeof(*m->counters[j]));
        m->histories[j] = calloc(count, 1);
        if (!m->counters[j] || !m->histories[j]) fail("out of memory");
        for (size_t i = 0; i < count; ++i) m->counters[j][i] = 1u << 31;
        for (size_t i = 0; i < 16u * 256u; ++i) m->indirect[j][i] = 1u << 31;
    }
    for (unsigned j = 0; j < 2; ++j) {
        size_t count = j ? slots : 256u;
        m->fast[j] = malloc(count * sizeof(*m->fast[j]));
        if (!m->fast[j]) fail("out of memory");
        for (size_t i = 0; i < count; ++i) m->fast[j][i] = 32768;
        for (size_t i = 0; i < 16u * 8u * 4u; ++i) m->ranks[j][i] = 1u << 31;
    }
    for (size_t i = 0; i < BWT2_SETS * BWT2_INPUTS; ++i) m->weights[i] = 1 << 14;
    for (size_t i = 0; i < 256u * BWT2_MIXERS; ++i) m->final_weights[i] = 65536 / BWT2_MIXERS;
    for (unsigned j = 0; j < 33; ++j) {
        uint16_t value = (uint16_t)(bwt2_squash(((int)j - 16) * 128) * 16);
        for (unsigned c = 0; c < 256; ++c) m->apm_order0[c * 33u + j] = value;
        for (unsigned c = 0; c < 65536; ++c) m->apm_order1[c * 33u + j] = value;
    }
    int next = 0;
    for (int x = -2047; x <= 2047; ++x) {
        int v = bwt2_squash(x);
        for (int j = next; j <= v; ++j) m->stretch[j] = (short)x;
        next = v + 1;
    }
    for (int j = next; j < 4096; ++j) m->stretch[j] = 2047;
    return m;
}

static void bwt2_model_free(Bwt2Model *m) {
    for (unsigned j = 0; j < BWT2_CONTEXTS; ++j) { free(m->counters[j]); free(m->histories[j]); }
    free(m->fast[0]); free(m->fast[1]); free(m);
}

static int bwt2_code(Bwt2Coder *c, int bit, int p) {
    uint32_t mid = c->low + (uint32_t)(((uint64_t)(c->high - c->low) * (uint32_t)p) >> 12);
    if (!c->output) bit = c->code <= mid;
    if (bit) c->high = mid; else c->low = mid + 1u;
    while (!((c->low ^ c->high) & 0xff000000u)) {
        if (c->output) {
            append_byte(c->output, (uint8_t)(c->high >> 24));
        } else {
            if (c->pos >= c->size) fail("truncated BWT model stream");
            c->code = (c->code << 8) | c->input[c->pos++];
        }
        c->low <<= 8;
        c->high = (c->high << 8) | 255u;
    }
    return bit;
}

static int bwt2_apm(uint16_t *t, int stretched, unsigned *slot) {
    int s = stretched + 2048, weight = s & 127, j = s >> 7;
    *slot = (unsigned)(j + (weight >> 6));
    return (t[j] * (128 - weight) + t[j + 1] * weight) >> 11;
}

static void bwt2_mix_update(int32_t *w, const int *x, int count, int err) {
    for (int i = 0; i < count; ++i) {
        int32_t v = w[i] + ((x[i] * err + (1 << 13)) >> 14);
        w[i] = v > (1 << 22) ? (1 << 22) : v < -(1 << 22) ? -(1 << 22) : v;
    }
}

/* Encodes symbol when coder->output is set; otherwise decodes and returns it. */
static unsigned bwt2_code_byte(Bwt2Model *m, Bwt2Coder *coder, unsigned symbol) {
    unsigned rb = bwt2_bucket(m->run), c1 = m->c1, c0 = 1;
    unsigned shift = 32u - m->table_bits;
    uint32_t mask = ((uint32_t)1 << m->table_bits) - 256u;
    m->bases[0] = 0;
    m->bases[1] = (bwt2_hash(1, c1) >> shift) & mask;
    m->bases[2] = (bwt2_hash(2, c1 | m->c2 << 8) >> shift) & mask;
    m->bases[3] = (bwt2_hash(3, c1 | rb << 8) >> shift) & mask;
    m->bases[4] = (bwt2_hash(4, c1 | m->d2 << 8) >> shift) & mask;
    m->bases[5] = (bwt2_hash(5, c1 | m->d2 << 8 | m->d3 << 16) >> shift) & mask;
    unsigned sets[BWT2_MIXERS];
    for (int k = 7; k >= 0; --k) {
        int x[BWT2_INPUTS], n = 0;
        uint32_t *counter[BWT2_CONTEXTS], *indirect[BWT2_CONTEXTS], *rank[2] = {0};
        uint8_t *history[BWT2_CONTEXTS];
        for (unsigned j = 0; j < BWT2_CONTEXTS; ++j) {
            uint32_t slot = m->bases[j] ^ c0;
            counter[j] = &m->counters[j][slot];
            history[j] = &m->histories[j][slot];
            indirect[j] = &m->indirect[j][rb << 8 | *history[j]];
            x[n++] = m->stretch[*counter[j] >> 20];
            if (j < 2) x[n++] = m->stretch[m->fast[j][slot] >> 4];
            x[n++] = m->stretch[*indirect[j] >> 20];
        }
        unsigned c1_prefix = ((c1 | 256u) >> (k + 1)) == c0;
        for (unsigned j = 0; j < 2; ++j) {
            unsigned candidate = (j ? m->d2 : c1) | 256u;
            if ((candidate >> (k + 1)) == c0) {
                unsigned expected = (candidate >> k) & 1u;
                unsigned prior = j ? c1_prefix : 0u;
                rank[j] = &m->ranks[j][((rb * 8u + (unsigned)k) * 2u + prior) * 2u + expected];
                x[n++] = m->stretch[*rank[j] >> 20];
            } else {
                x[n++] = 0;
            }
        }
        x[n++] = 256;
        sets[0] = c0;
        sets[1] = 256u + c1;
        sets[2] = 512u + (rb << 8 | c0);
        sets[3] = 512u + 4096u + rb;
        int mixed[BWT2_MIXERS], probability[BWT2_MIXERS];
        int64_t total = 0;
        int32_t *final = &m->final_weights[c0 * BWT2_MIXERS];
        for (unsigned j = 0; j < BWT2_MIXERS; ++j) {
            const int32_t *w = &m->weights[sets[j] * BWT2_INPUTS];
            int64_t dot = 0;
            for (int i = 0; i < BWT2_INPUTS; ++i) dot += (int64_t)x[i] * w[i];
            mixed[j] = bwt2_clamp(dot >> 16);
            probability[j] = bwt2_squash(mixed[j]);
            total += (int64_t)mixed[j] * final[j];
        }
        int final_stretch = bwt2_clamp(total >> 16);
        int final_probability = bwt2_squash(final_stretch);
        unsigned slot0, slot1;
        uint16_t *apm0 = &m->apm_order0[c0 * 33u];
        uint16_t *apm1 = &m->apm_order1[(c1 << 8 | c0) * 33u];
        int stretched = m->stretch[final_probability];
        int p = (bwt2_apm(apm0, stretched, &slot0) + bwt2_apm(apm1, stretched, &slot1)) >> 1;
        if (p < 1) p = 1;
        if (p > 4095) p = 4095;
        int bit = bwt2_code(coder, (int)((symbol >> k) & 1u), p);

        for (unsigned j = 0; j < BWT2_MIXERS; ++j)
            bwt2_mix_update(&m->weights[sets[j] * BWT2_INPUTS], x, BWT2_INPUTS,
                            ((bit << 12) - probability[j]) * 6);
        bwt2_mix_update(final, mixed, BWT2_MIXERS, ((bit << 12) - final_probability) * 4);
        int target = (bit << 16) + (bit << 7) - bit - bit;
        apm0[slot0] = (uint16_t)(apm0[slot0] + ((target - apm0[slot0]) >> 7));
        apm1[slot1] = (uint16_t)(apm1[slot1] + ((target - apm1[slot1]) >> 7));
        for (unsigned j = 0; j < BWT2_CONTEXTS; ++j) {
            bwt2_counter_update(counter[j], bit, BWT2_LIMIT);
            bwt2_counter_update(indirect[j], bit, BWT2_LIMIT);
            unsigned h = *history[j] ? *history[j] : 1u;
            h = (h << 1) | (unsigned)bit;
            *history[j] = (uint8_t)(h >= 256u ? (h & 127u) | 128u : h);
            if (j < 2) {
                uint16_t *f = &m->fast[j][m->bases[j] ^ c0];
                *f = (uint16_t)(*f + (((bit << 16) - *f) >> 4));
            }
        }
        for (unsigned j = 0; j < 2; ++j)
            if (rank[j]) bwt2_counter_update(rank[j], bit, BWT2_RANK_LIMIT);
        c0 = (c0 << 1) | (unsigned)bit;
    }
    unsigned c = c0 & 255u;
    if (c == c1) { ++m->run; }
    else { m->run = 1; m->d3 = m->d2; m->d2 = c1; }
    m->c2 = c1; m->c1 = c;
    return c;
}

static Buffer bwt2_encode(const uint8_t *input, size_t size) {
    uint32_t primary = 0;
    Buffer last = bwt_transform(input, size, &primary);
    Buffer output = {0};
    append_u32le(&output, primary);
    Bwt2Coder coder = {0, UINT32_MAX, 0, &output, NULL, 0, 0};
    Bwt2Model *model = bwt2_model_new(size);
    for (size_t i = 0; i < size; ++i) bwt2_code_byte(model, &coder, last.data[i]);
    for (unsigned i = 0; i < 4; ++i) append_byte(&output, (uint8_t)(coder.low >> (24u - 8u * i)));
    bwt2_model_free(model);
    free(last.data);
    return output;
}

static Buffer bwt2_decode(const uint8_t *input, size_t size, size_t decoded_size) {
    if (size < 8 || !decoded_size || decoded_size > WHOLE_BLOCK_SIZE)
        fail("invalid BWT model payload");
    uint32_t primary = read_u32le(input);
    if (primary >= decoded_size) fail("invalid BWT primary index");
    Bwt2Coder coder = {0, UINT32_MAX, 0, NULL, input + 4, size - 4, 4};
    for (unsigned i = 0; i < 4; ++i) coder.code = (coder.code << 8) | input[4 + i];
    Buffer last = {0};
    reserve(&last, decoded_size);
    Bwt2Model *model = bwt2_model_new(decoded_size);
    for (size_t i = 0; i < decoded_size; ++i)
        last.data[i] = (uint8_t)bwt2_code_byte(model, &coder, 0);
    last.size = decoded_size;
    bwt2_model_free(model);
    if (coder.pos != coder.size) fail("trailing BWT model data");
    return bwt_inverse(last, primary);
}

/* Mode 29: the mode-26 transform and coder with a cheaper model (EXP-0085).
 * Each byte first codes one "equals the previous byte" decision. Only a miss
 * codes a literal, along a per-block canonical prefix code built from the
 * literal frequencies, so frequent literals take few binary decisions. A branch
 * to the previous byte's leaf is never taken and costs no decision.
 * Payload: u32 primary row, 32-byte literal bitmap, one 4-bit code length per
 * literal value (low nibble first), then the arithmetic code.
 * Each hashed context owns a 256-slot window: slot 0 predicts the run flag and
 * slot `node` the branch at internal tree node `node` (1..255). A slot packs an
 * 18-bit probability, a 6-bit hit count and the last up to 7 bits seen there.
 */
#define BWT3_CONTEXTS 4                /* order 0, (c1,run), (c1,d2), (c1,d2,d3) */
#define BWT3_INPUTS 12                 /* 4 contexts x 2, 2 fast, rank, bias */
#define BWT3_LIMIT 60
#define BWT3_MAX_LENGTH 15
#define BWT3_WEIGHT_LIMIT (1 << 19)

typedef struct {
    uint32_t *slots[BWT3_CONTEXTS];
    uint16_t *fast[2];                 /* fast-rate order-0 and order-1 */
    uint32_t flag_indirect[BWT3_CONTEXTS][16 * 256];
    uint32_t indirect[BWT3_CONTEXTS][16 * 256];
    uint32_t ranks[16 * 16 * 2];
    uint32_t rates[BWT3_LIMIT + 1];
    int32_t flag_weights[256 * BWT3_INPUTS];
    int32_t weights[(256 + 16) * BWT3_INPUTS];
    int32_t final_weights[256 * 2];
    uint16_t flag_apm[256 * 33];
    uint16_t apm_order0[256 * 33];
    uint16_t apm_order1[65536 * 33];
    short stretch[4096], squash[4096];
    uint8_t buckets[1024];
    uint16_t child[256][2];            /* internal node, or 256 | value for a leaf */
    uint16_t code[256];
    uint8_t length[256];
    unsigned table_bits, leaves, c1, d2, d3, flags;
    uint32_t run;
} Bwt3Model;

static inline uint32_t bwt3_update(const Bwt3Model *m, uint32_t v, int bit) {
    uint32_t n = (v >> 8) & 63u;
    int32_t p = (int32_t)(v >> 14);
    p += (int32_t)(((((int64_t)bit << 18) - p) * m->rates[n]) >> 16);
    n += n < BWT3_LIMIT;
    return (uint32_t)p << 14 | n << 8 | (v & 255u);
}

static inline uint32_t bwt3_update_history(const Bwt3Model *m, uint32_t v, int bit) {
    unsigned h = v & 255u;
    h = ((h ? h : 1u) << 1) | (unsigned)bit;
    h = h >= 256u ? (h & 127u) | 128u : h;
    return (bwt3_update(m, v, bit) & ~255u) | h;
}

/* Bit-history counters adapt at a fixed rate; their count and history bits stay 0. */
static inline uint32_t bwt3_update_indirect(uint32_t v, int bit) {
    int32_t p = (int32_t)(v >> 14);
    p += ((bit << 18) - p) >> 5;
    return (uint32_t)p << 14;
}

static inline int bwt3_stretch(const Bwt3Model *m, uint32_t v) {
    return m->stretch[v >> 20];
}

static inline int bwt3_squash(const Bwt3Model *m, int d) {
    return m->squash[(d > 2047 ? 2047 : d < -2047 ? -2047 : d) + 2048];
}

static inline int bwt3_dot(const int32_t *restrict w, const int32_t *restrict x) {
    int32_t sum = 0;
    for (int i = 0; i < BWT3_INPUTS; ++i) sum += (x[i] * w[i]) >> 8;
    sum >>= 8;
    return sum > 2047 ? 2047 : sum < -2047 ? -2047 : sum;
}

static inline void bwt3_train(int32_t *restrict w, const int32_t *restrict x, int err) {
    for (int i = 0; i < BWT3_INPUTS; ++i) {
        int32_t v = w[i] + ((x[i] * err + (1 << 13)) >> 14);
        w[i] = v > BWT3_WEIGHT_LIMIT ? BWT3_WEIGHT_LIMIT
             : v < -BWT3_WEIGHT_LIMIT ? -BWT3_WEIGHT_LIMIT : v;
    }
}

static inline int bwt3_apm(const uint16_t *t, int j, int weight) {
    return (t[j] * (128 - weight) + t[j + 1] * weight) >> 11;
}

static inline void bwt3_apm_update(uint16_t *t, unsigned slot, int bit) {
    int target = (bit << 16) + (bit << 7) - bit - bit;
    t[slot] = (uint16_t)(t[slot] + ((target - t[slot]) >> 7));
}

static Bwt3Model *bwt3_model_new(size_t block_size) {
    Bwt3Model *m = calloc(1, sizeof(*m));
    if (!m) fail("out of memory");
    m->table_bits = 12;
    while (m->table_bits < 20 && ((size_t)1 << m->table_bits) < block_size * 8u)
        ++m->table_bits;
    size_t slots = (size_t)1 << m->table_bits;
    for (unsigned j = 0; j < BWT3_CONTEXTS; ++j) {
        size_t count = j ? slots : 256u;
        m->slots[j] = malloc(count * sizeof(*m->slots[j]));
        if (!m->slots[j]) fail("out of memory");
        for (size_t i = 0; i < count; ++i) m->slots[j][i] = 1u << 31;
        for (size_t i = 0; i < 16u * 256u; ++i)
            m->flag_indirect[j][i] = m->indirect[j][i] = 1u << 31;
    }
    for (unsigned j = 0; j < 2; ++j) {
        size_t count = j ? slots : 256u;
        m->fast[j] = malloc(count * sizeof(*m->fast[j]));
        if (!m->fast[j]) fail("out of memory");
        for (size_t i = 0; i < count; ++i) m->fast[j][i] = 32768;
    }
    for (size_t i = 0; i < 16u * 16u * 2u; ++i) m->ranks[i] = 1u << 31;
    for (unsigned n = 0; n <= BWT3_LIMIT; ++n) m->rates[n] = 131072u / (2u * n + 3u);
    for (size_t i = 0; i < 256u * BWT3_INPUTS; ++i) m->flag_weights[i] = 1 << 14;
    for (size_t i = 0; i < (256u + 16u) * BWT3_INPUTS; ++i) m->weights[i] = 1 << 14;
    for (size_t i = 0; i < 256u * 2u; ++i) m->final_weights[i] = 1 << 15;
    for (unsigned j = 0; j < 33; ++j) {
        uint16_t value = (uint16_t)(bwt2_squash(((int)j - 16) * 128) * 16);
        for (unsigned c = 0; c < 256; ++c)
            m->flag_apm[c * 33u + j] = m->apm_order0[c * 33u + j] = value;
        for (unsigned c = 0; c < 65536; ++c) m->apm_order1[c * 33u + j] = value;
    }
    int next = 0;
    for (int x = -2047; x <= 2047; ++x) {
        int v = bwt2_squash(x);
        for (int j = next; j <= v; ++j) m->stretch[j] = (short)x;
        next = v + 1;
    }
    for (int j = next; j < 4096; ++j) m->stretch[j] = 2047;
    for (int d = -2048; d < 2048; ++d) m->squash[d + 2048] = (short)bwt2_squash(d);
    for (uint32_t r = 0; r < 1024; ++r) m->buckets[r] = (uint8_t)bwt2_bucket(r);
    return m;
}

static void bwt3_model_free(Bwt3Model *m) {
    for (unsigned j = 0; j < BWT3_CONTEXTS; ++j) free(m->slots[j]);
    free(m->fast[0]); free(m->fast[1]); free(m);
}

/* Literal code lengths: Huffman over the block's literal frequencies, with the
 * frequencies halved until no code exceeds BWT3_MAX_LENGTH. A single literal
 * value gets a one-bit sibling, so every non-empty code has two or more leaves. */
static void bwt3_code_lengths(const uint32_t *frequency, uint8_t *length) {
    uint32_t weight[511], f[256];
    uint16_t parent[511];
    unsigned present = 0, only = 0;
    memset(length, 0, 256);
    for (unsigned c = 0; c < 256; ++c) if (frequency[c]) { ++present; only = c; }
    if (!present) return;
    if (present == 1) { length[only] = length[only ^ 1u] = 1; return; }
    memcpy(f, frequency, sizeof(f));
    for (;;) {
        unsigned nodes = 0, leaf[256], count = 0;
        uint8_t active[511];
        for (unsigned c = 0; c < 256; ++c)
            if (f[c]) { leaf[count++] = c; weight[nodes] = f[c]; active[nodes++] = 1; }
        for (unsigned merged = 1; merged < count; ++merged) {
            unsigned a = 511, b = 511;
            for (unsigned i = 0; i < nodes; ++i) {
                if (!active[i]) continue;
                if (a == 511 || weight[i] < weight[a]) { b = a; a = i; }
                else if (b == 511 || weight[i] < weight[b]) b = i;
            }
            active[a] = active[b] = 0;
            parent[a] = parent[b] = (uint16_t)nodes;
            weight[nodes] = weight[a] + weight[b];
            active[nodes++] = 1;
        }
        unsigned longest = 0;
        for (unsigned i = 0; i < count; ++i) {
            unsigned depth = 0;
            for (unsigned j = i; j != nodes - 1u; j = parent[j]) ++depth;
            length[leaf[i]] = (uint8_t)depth;
            if (depth > longest) longest = depth;
        }
        if (longest <= BWT3_MAX_LENGTH) return;
        for (unsigned c = 0; c < 256; ++c) if (f[c]) f[c] = (f[c] >> 1) | 1u;
    }
}

/* Canonical prefix code from code lengths; returns 0 unless the code is complete. */
static int bwt3_build_tree(Bwt3Model *m, const uint8_t *length) {
    uint32_t kraft = 0;
    unsigned leaves = 0;
    for (unsigned c = 0; c < 256; ++c) {
        if (!length[c]) continue;
        if (length[c] > BWT3_MAX_LENGTH) return 0;
        kraft += 1u << (BWT3_MAX_LENGTH - length[c]);
        ++leaves;
    }
    memcpy(m->length, length, 256);
    memset(m->child, 0, sizeof(m->child));
    m->leaves = leaves;
    if (!leaves) return 1;
    if (kraft != 1u << BWT3_MAX_LENGTH) return 0;
    unsigned next_code = 0, next_node = 2;
    for (unsigned bits = 1; bits <= BWT3_MAX_LENGTH; ++bits) {
        for (unsigned c = 0; c < 256; ++c) {
            if (length[c] != bits) continue;
            m->code[c] = (uint16_t)next_code++;
            unsigned node = 1;
            for (unsigned d = bits - 1u; d > 0; --d) {
                unsigned b = (m->code[c] >> d) & 1u;
                if (!m->child[node][b]) m->child[node][b] = (uint16_t)next_node++;
                node = m->child[node][b];
            }
            m->child[node][m->code[c] & 1u] = (uint16_t)(256u | c);
        }
        next_code <<= 1;
    }
    return 1;
}

/* A byte is a literal when it differs from the previous one (initially 0). */
static void bwt3_literal_frequencies(const uint8_t *data, size_t size, uint32_t *frequency) {
    unsigned previous = 0;
    memset(frequency, 0, 256 * sizeof(*frequency));
    for (size_t i = 0; i < size; ++i) {
        if (data[i] != previous) ++frequency[data[i]];
        previous = data[i];
    }
}

/* bwt2_code with the direction fixed at compile time. */
static inline int bwt3_code(Bwt2Coder *c, int bit, int p, const int decode) {
    uint32_t mid = c->low + (uint32_t)(((uint64_t)(c->high - c->low) * (uint32_t)p) >> 12);
    if (decode) bit = c->code <= mid;
    if (bit) c->high = mid; else c->low = mid + 1u;
    while (!((c->low ^ c->high) & 0xff000000u)) {
        if (!decode) {
            append_byte(c->output, (uint8_t)(c->high >> 24));
        } else {
            if (c->pos >= c->size) fail("truncated BWT model stream");
            c->code = (c->code << 8) | c->input[c->pos++];
        }
        c->low <<= 8;
        c->high = (c->high << 8) | 255u;
    }
    return bit;
}

/* Encodes symbol when decode is 0; otherwise decodes and returns it. */
static inline unsigned bwt3_code_byte(Bwt3Model *m, Bwt2Coder *coder, unsigned symbol,
                                      const int decode) {
    unsigned rb = m->run < 1024u ? m->buckets[m->run] : 14u, c1 = m->c1, d2 = m->d2;
    unsigned history = m->flags & 15u, shift = 32u - m->table_bits;
    uint32_t mask = ((uint32_t)1 << m->table_bits) - 256u;
    uint32_t order1 = (bwt2_hash(1, c1) >> shift) & mask;
    uint32_t *window[BWT3_CONTEXTS], *indirect[BWT3_CONTEXTS];
    window[0] = m->slots[0];
    window[1] = m->slots[1] + ((bwt2_hash(3, c1 | rb << 8) >> shift) & mask);
    window[2] = m->slots[2] + ((bwt2_hash(4, c1 | d2 << 8) >> shift) & mask);
    window[3] = m->slots[3] + ((bwt2_hash(5, c1 | d2 << 8 | m->d3 << 16) >> shift) & mask);
    int32_t x[BWT3_INPUTS] = {0};
    int bit;
    {
        for (unsigned j = 0; j < BWT3_CONTEXTS; ++j) {
            uint32_t v = window[j][0];
            indirect[j] = &m->flag_indirect[j][rb << 8 | (v & 255u)];
            x[2 * j] = bwt3_stretch(m, v);
            x[2 * j + 1] = bwt3_stretch(m, *indirect[j]);
        }
        x[2 * BWT3_CONTEXTS] = 256;
        int32_t *w = &m->flag_weights[(rb << 4 | history) * BWT3_INPUTS];
        uint16_t *apm = &m->flag_apm[(rb << 4 | history) * 33u];
        int st = bwt3_dot(w, x), mixed = bwt3_squash(m, st);
        int s = st + 2048, weight = s & 127, j = s >> 7;
        int p = (bwt3_apm(apm, j, weight) + mixed) >> 1;
        p = p < 1 ? 1 : p > 4095 ? 4095 : p;
        bit = bwt3_code(coder, symbol == c1, p, decode);
        bwt3_train(w, x, ((bit << 12) - mixed) * 4);
        bwt3_apm_update(apm, (unsigned)(j + (weight >> 6)), bit);
        for (unsigned k = 0; k < BWT3_CONTEXTS; ++k) {
            window[k][0] = bwt3_update_history(m, window[k][0], bit);
            *indirect[k] = bwt3_update_indirect(*indirect[k], bit);
        }
    }
    m->flags = (m->flags << 1) | (unsigned)bit;
    unsigned c = c1;
    if (!bit) {
        if (!m->leaves) fail("invalid BWT literal");
        uint16_t *fast0 = m->fast[0], *fast1 = m->fast[1] + order1;
        unsigned node = 1, depth = 0, path = 0, run_leaf = 256u | c1;
        unsigned d2_length = m->length[d2], d2_code = m->code[d2];
        unsigned code = decode ? 0u : m->code[symbol], length = decode ? 0u : m->length[symbol];
        for (;;) {
            unsigned next;
            if (m->child[node][0] == run_leaf) { next = m->child[node][1]; bit = 1; }
            else if (m->child[node][1] == run_leaf) { next = m->child[node][0]; bit = 0; }
            else {
                uint32_t *rank = NULL;
                for (unsigned j = 0; j < BWT3_CONTEXTS; ++j) {
                    uint32_t v = window[j][node];
                    indirect[j] = &m->indirect[j][rb << 8 | (v & 255u)];
                    x[2 * j] = bwt3_stretch(m, v);
                    x[2 * j + 1] = bwt3_stretch(m, *indirect[j]);
                }
                x[2 * BWT3_CONTEXTS] = m->stretch[fast0[node] >> 4];
                x[2 * BWT3_CONTEXTS + 1] = m->stretch[fast1[node] >> 4];
                x[2 * BWT3_CONTEXTS + 2] = 0;
                /* Symbol-rank input: is the previous distinct byte still on this path? */
                if (d2_length > depth && (d2_code >> (d2_length - depth)) == path) {
                    rank = &m->ranks[(rb * 16u + depth) * 2u + ((d2_code >> (d2_length - depth - 1u)) & 1u)];
                    x[2 * BWT3_CONTEXTS + 2] = bwt3_stretch(m, *rank);
                }
                x[2 * BWT3_CONTEXTS + 3] = 256;
                int32_t *w0 = &m->weights[node * BWT3_INPUTS];
                int32_t *w1 = &m->weights[(256u + rb) * BWT3_INPUTS];
                int32_t *final = &m->final_weights[node * 2u];
                int s0 = bwt3_dot(w0, x), p0 = bwt3_squash(m, s0);
                int s1 = bwt3_dot(w1, x), p1 = bwt3_squash(m, s1);
                int sf = (int)(((int64_t)s0 * final[0] + (int64_t)s1 * final[1]) >> 16);
                sf = sf > 2047 ? 2047 : sf < -2047 ? -2047 : sf;
                int pf = bwt3_squash(m, sf);
                uint16_t *a0 = &m->apm_order0[node * 33u], *a1 = &m->apm_order1[(c1 << 8 | node) * 33u];
                int s = sf + 2048, weight = s & 127, j = s >> 7;
                unsigned slot = (unsigned)(j + (weight >> 6));
                int p = (bwt3_apm(a0, j, weight) + bwt3_apm(a1, j, weight)) >> 1;
                p = p < 1 ? 1 : p > 4095 ? 4095 : p;
                bit = bwt3_code(coder, decode ? 0 : (int)((code >> (length - depth - 1u)) & 1u), p, decode);
                int target = bit << 12, err = (target - pf) * 2;
                bwt3_train(w0, x, (target - p0) * 8);
                bwt3_train(w1, x, (target - p1) * 8);
                final[0] += (s0 * err + (1 << 13)) >> 14;
                final[1] += (s1 * err + (1 << 13)) >> 14;
                bwt3_apm_update(a0, slot, bit);
                bwt3_apm_update(a1, slot, bit);
                for (unsigned i = 0; i < BWT3_CONTEXTS; ++i) {
                    window[i][node] = bwt3_update_history(m, window[i][node], bit);
                    *indirect[i] = bwt3_update_indirect(*indirect[i], bit);
                }
                fast0[node] = (uint16_t)(fast0[node] + (((bit << 16) - fast0[node]) >> 2));
                fast1[node] = (uint16_t)(fast1[node] + (((bit << 16) - fast1[node]) >> 2));
                if (rank) *rank = bwt3_update(m, *rank, bit);
                next = m->child[node][bit];
            }
            path = (path << 1) | (unsigned)bit;
            ++depth;
            if (next & 256u) { c = next & 255u; break; }
            node = next;
        }
    }
    if (c == c1) { ++m->run; }
    else { m->run = 1; m->d3 = d2; m->d2 = c1; }
    m->c1 = c;
    return c;
}

static void bwt3_encode_symbols(Bwt3Model *m, Bwt2Coder *coder, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) bwt3_code_byte(m, coder, data[i], 0);
}

static void bwt3_decode_symbols(Bwt3Model *m, Bwt2Coder *coder, uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; ++i) data[i] = (uint8_t)bwt3_code_byte(m, coder, 0, 1);
}

static Buffer bwt3_encode(const uint8_t *input, size_t size) {
    uint32_t primary = 0, frequency[256];
    uint8_t length[256], present[32] = {0};
    Buffer last = bwt_transform(input, size, &primary);
    Buffer output = {0};
    append_u32le(&output, primary);
    bwt3_literal_frequencies(last.data, size, frequency);
    bwt3_code_lengths(frequency, length);
    for (unsigned c = 0; c < 256; ++c) if (length[c]) present[c >> 3] |= (uint8_t)(1u << (c & 7u));
    append_bytes(&output, present, sizeof(present));
    unsigned pending = 0, half = 0;
    for (unsigned c = 0; c < 256; ++c) {
        if (!length[c]) continue;
        if (half) append_byte(&output, (uint8_t)(pending | (unsigned)length[c] << 4));
        else pending = length[c];
        half ^= 1u;
    }
    if (half) append_byte(&output, (uint8_t)pending);
    Bwt3Model *model = bwt3_model_new(size);
    if (!bwt3_build_tree(model, length)) fail("invalid BWT literal code");
    Bwt2Coder coder = {0, UINT32_MAX, 0, &output, NULL, 0, 0};
    bwt3_encode_symbols(model, &coder, last.data, size);
    for (unsigned i = 0; i < 4; ++i) append_byte(&output, (uint8_t)(coder.low >> (24u - 8u * i)));
    bwt3_model_free(model);
    free(last.data);
    return output;
}

static Buffer bwt3_decode(const uint8_t *input, size_t size, size_t decoded_size) {
    if (size < 4 + 32 + 4 || !decoded_size || decoded_size > WHOLE_BLOCK_SIZE)
        fail("invalid BWT model payload");
    uint32_t primary = read_u32le(input);
    if (primary >= decoded_size) fail("invalid BWT primary index");
    const uint8_t *present = input + 4;
    size_t pos = 4 + 32;
    uint8_t length[256];
    unsigned half = 0;
    for (unsigned c = 0; c < 256; ++c) {
        length[c] = 0;
        if (!(present[c >> 3] >> (c & 7u) & 1u)) continue;
        if (pos >= size) fail("truncated BWT literal code");
        length[c] = (uint8_t)(half ? input[pos++] >> 4 : input[pos] & 15u);
        if (!length[c]) fail("invalid BWT literal code");
        half ^= 1u;
    }
    if (half && input[pos++] >> 4) fail("invalid BWT literal code");
    if (size - pos < 4) fail("truncated BWT model stream");
    Bwt3Model *model = bwt3_model_new(decoded_size);
    if (!bwt3_build_tree(model, length)) fail("invalid BWT literal code");
    Bwt2Coder coder = {0, UINT32_MAX, 0, NULL, input + pos, size - pos, 4};
    for (unsigned i = 0; i < 4; ++i) coder.code = (coder.code << 8) | input[pos + i];
    Buffer last = {0};
    reserve(&last, decoded_size);
    bwt3_decode_symbols(model, &coder, last.data, decoded_size);
    last.size = decoded_size;
    bwt3_model_free(model);
    if (coder.pos != coder.size) fail("trailing BWT model data");
    return bwt_inverse(last, primary);
}

/* Standalone research presets (mode 25 cbwt, mode 26 cbwt2, mode 29 cbwt3), with the same
 * framing and raw fallback as cmax. */
static Buffer compress_bwt_blocked(const uint8_t *input, size_t size, uint8_t mode) {
    Buffer output = {0};
    const uint8_t header[8] = {'B', 'I', 'T', 'E', 2, 0, 0, 0};
    append_bytes(&output, header, sizeof(header));
    append_u64le(&output, size);
    for (size_t pos = 0; pos < size;) {
        size_t block = size - pos;
        if (block > WHOLE_BLOCK_SIZE) block = WHOLE_BLOCK_SIZE;
        Buffer coded = mode == 29 ? bwt3_encode(input + pos, block)
                     : mode == 26 ? bwt2_encode(input + pos, block)
                     : bwt_encode(input + pos, block);
        int selected = coded.size < block;
        append_byte(&output, selected ? mode : 0);
        append_u32le(&output, (uint32_t)block);
        append_u32le(&output, (uint32_t)(selected ? coded.size : block));
        append_bytes(&output, selected ? coded.data : input + pos,
                     selected ? coded.size : block);
        free(coded.data);
        pos += block;
    }
    return output;
}
